/*
 * examon_metric.c
 *
 *  Created on: 09.08.2018
 *      Author: jitschin
 *
 * Copyright (c) 2018, Technische Universität Dresden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions
 *    and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of
 * conditions and the following disclaimer in the documentation and/or other materials provided with
 * the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
extern "C" {
#include <libgen.h>
#include <string.h>
}

#include <mosquittopp.h>
#include <scorep/chrono/chrono.hpp>

#include <utility>
#include <vector>

#include "examon_mqtt_path.cpp"
#include <string>

#include "include_once.hpp"

class examon_metric
{
private:
    std::int32_t id;  /**< the id of this metric, with which it is known to Score-P */
    std::string name; /**< the short name of this metric e.g. "cpu/0/erg_pkg" */
    examon_mqtt_path *channels;  /**< the associated channels/topics for the current host */
    std::string full_topic;  /**< the full spelled out topic, it is often quite long */
    double metric_value;  /**< the last read value */
    double metric_timestamp;  /**< the last read Timestamp */
    double metric_elapsed;  /**< the time that elapsed between the last two Timestamps */
    std::int64_t metric_iterations;  /**< how many values were read in total of this metric */
    std::int64_t metric_topic_count;  /**< how many values were read with the same Timestamp at the beginning of measurement */
    double metric_accumulated;  /**< the latest accumulated value */
    double scale_mul = 1.00; /**< multiplicator to scale the result */
    std::int64_t metric_sub_iterations;  /**< how many values with the same timestamp were just received */
    ACCUMULATION_STRATEGY acc_strategy;  /**< how to add/subtract/calculate the accumulated value */
    EXAMON_METRIC_TYPE metric_type;  /**< the kind of metric we are treating herein */
    OUTPUT_DATATYPE metric_datatype;  /**< which datatype to report to Score-P */
    double erg_unit;  /**< stored erg_unit with which to multiply the raw erg_* values */
    bool do_gather_data; /**< whether we need to store the received values (e.g. for the async plugin) */
    std::vector<std::pair<scorep::chrono::ticks, double>> gathered_data; /**< stored values */


public:
    /**
     * returns the output datatype
     */
    OUTPUT_DATATYPE get_output_datatype()
    {
        return metric_datatype;
    }
    /**
     * update the erg_unit with which to multiply erg_* units
     */
    void set_erg_unit(double param_erg_unit)
    {
        erg_unit = param_erg_unit;
    }
    /**
     * returns the full MQTT/Examon topic for this metric
     */
    std::string get_full_topic()
    {
        return full_topic;
    }
    /**
     * return the short name of this metric
     */
    std::string get_name()
    {
        return name;
    }
    /**
     * set whether to gather the read values
     *
     * used when finishing the async plugin to call an end to the data gathering
     */
    void set_gather_data(bool param_do_gather)
    {
        do_gather_data = param_do_gather;
    }
    /**
     * return my id
     */
    std::int32_t get_id()
    {
        return id;
    }
    /**
     * returns a pointer to the data that was gathered with this metric
     */
    std::vector<std::pair<scorep::chrono::ticks, double>>* get_gathered_data()
    {
        return &gathered_data;
    }
    /**
     * Initialize an Examon metric
     * @param param_id the id provided to Score-P for this metric
     * @param param_name the short name/short topic for this metric
     * @param param_channels a pointer to the corresponding MQTT/Examon topic descriptor
     * @param param_gather whether to retain read out values and timestamps (e.g. for async plugin to be read out later)
     */
    examon_metric(std::int32_t param_id, std::string param_name, examon_mqtt_path *param_channels,
                  bool param_gather)
    {
        id = param_id;
        /* configurable accumulation strategy, TODO: also have a heuristic to determine this setting
         * if not explicitly specified */
        acc_strategy = ACCUMULATION_AVG;
        metric_datatype = OUTPUT_DATATYPE::DOUBLE;
        int semicolon_pos = param_name.find_first_of(';');
        if (std::string::npos != semicolon_pos)
        {
            parse_metric_options(param_name.substr(semicolon_pos + 1, std::string::npos).c_str(), acc_strategy, metric_datatype, scale_mul);
            param_name = param_name.substr(0, semicolon_pos);
        }
        name = param_name;
        channels = param_channels;
        full_topic = channels->get_data_topic(name);
        char* metric_basename = basename((char*)name.c_str());
        metric_type = parse_metric_type(metric_basename);

        metric_value = -1.00;
        metric_timestamp = 0.00;
        metric_elapsed = 0.00;
        metric_iterations = 0;
        metric_topic_count = 1;
        metric_accumulated = -1.00;
        metric_sub_iterations = 0;

        do_gather_data = param_gather;
    }

    /**
     * whether the given topic matches this metric's topic/name
     *
     * @param incoming_topic a char* pointer to a C-style-string
     * @see handle_message()
     */
    bool metric_matches(char* incoming_topic)
    {
        bool topic_matches = false;
        mosqpp::topic_matches_sub(full_topic.c_str(), incoming_topic, &topic_matches);
        return topic_matches;
    }
    /**
     * Handle an incoming MQTT message, to be called after metric_matches()
     * @see metric_matches()
     */
    void handle_message(char* incoming_topic, char* incoming_payload, int payloadlen)
    {
        double read_value = -1;
        double read_timestamp = -1;
        int values_read = sscanf(incoming_payload, "%lf;%lf", &read_value, &read_timestamp);
        if (2 == values_read)
        {
            if (read_timestamp != metric_timestamp)
            {
                metric_elapsed = read_timestamp - metric_timestamp;

                metric_value = read_value; // - put value in metricValues
                ++metric_iterations;
                metric_sub_iterations = 1;
                if (do_gather_data)
                {
                    if (1 < metric_iterations && 1 == metric_topic_count)
                    {
                        push_latest_value(false);
                    }
                }
            }
            else
            {
                ++metric_sub_iterations;
                if (1 == metric_iterations)
                {
                    ++metric_topic_count;
                }
                else
                {
                    // TODO: put all this logic in a separate class for the metric
                    //         optional: implement fancy pthread_mutex_locking for it

                    // accumulate strategy
                    // at this point metricTopicCount contains a sensible number of actually
                    //   subscribed to metrics

                    // here is the first duplicate, i.e. timestamps are equal
                    // so the current preceding value was already written to metricValues[i]
                    bool completed_cycle = metric_sub_iterations == metric_topic_count;
                    switch (acc_strategy)
                    {
                    case ACCUMULATION_AVG:
                        metric_value += read_value;
                        if (completed_cycle)
                        {
                            metric_accumulated = metric_value / metric_topic_count;
                        }
                        break;
                    case ACCUMULATION_SUM:
                        metric_value += read_value;
                        if (completed_cycle)
                            metric_accumulated = metric_value;
                        break;
                    case ACCUMULATION_MIN:
                        if (metric_value > read_value)
                            metric_value = read_value;
                        if (completed_cycle)
                            metric_accumulated = metric_value;
                        break;
                    case ACCUMULATION_MAX:
                        if (metric_value < read_value)
                            metric_value = read_value;
                        if (completed_cycle)
                            metric_accumulated = metric_value;
                        break;
                    }
                    if (completed_cycle && do_gather_data)
                    {
                        push_latest_value(true);
                    }
                }
            }
            metric_timestamp = read_timestamp;

        }
    }
    /**
     * return whether this metric has so far read out a valid metric value
     */
    bool has_value()
    {
        if (1 < metric_iterations)
        {
            if (metric_type != EXAMON_METRIC_TYPE::ENERGY || 0 < erg_unit)
            {
                if (1 < metric_topic_count)
                {
                    return -1.00 != metric_accumulated;
                }
                else
                {
                    return true;
                }
            }
        }
        return false;
    }
    /**
     * returns the last read value, adjusted by erg_unit if necessary
     */
    double get_latest_value()
    {
        double return_value = 0.00;
        if (1 < metric_topic_count)
        {
            return_value = metric_accumulated;
        }
        else
        {
            return_value = metric_value;
        }
        if (metric_type == EXAMON_METRIC_TYPE::ENERGY)
        {
            if (0 < erg_unit)
            {
                return_value = return_value * erg_unit;
            }
        }

        return return_value * scale_mul;
    }
    /**
     * internally used for when a value is to be quickly stored
     */
    void push_latest_value(bool accumulated)
    {
        // std::chrono::system_clock::time_point
        scorep::chrono::ticks now_ticks = scorep::chrono::measurement_clock::now();
        double write_metric = 0.00;
        if (accumulated)
            write_metric = metric_accumulated;
        else
            write_metric = metric_value;
        if (metric_type == EXAMON_METRIC_TYPE::ENERGY)
        {
            if (0 < erg_unit)
            {

                gathered_data.push_back(
                    std::pair<scorep::chrono::ticks, double>(now_ticks, write_metric * erg_unit * scale_mul));
            }
        }
        else
        {
            gathered_data.push_back(
                std::pair<scorep::chrono::ticks, double>(now_ticks, write_metric * scale_mul));
        }
    }
};
