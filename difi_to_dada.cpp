#include <zmq.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <complex>

// VRT
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <vrt/vrt_read.h>
#include <vrt/vrt_string.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>

// #include <complex.h>

// DADA
#include <sstream>
#include <dada_def.h>
#include <dada_hdu.h>
#include <multilog.h>
#include <ipcio.h>
#include <iomanip>
#include <ascii_header.h>
// #include "interleave.h"
// END DADA

#include "difi-tools.h"

namespace po = boost::program_options;

static bool stop_signal_called = false;
void sig_int_handler(int)
{
    stop_signal_called = true;
}

template <typename samp_type> inline float get_abs_val(samp_type t)
{
    return std::fabs(t);
}

inline float get_abs_val(std::complex<int16_t> t)
{
    return std::fabs(t.real());
}

inline float get_abs_val(std::complex<int8_t> t)
{
    return std::fabs(t.real());
}

int main(int argc, char* argv[])
{
    // variables to be set by po
    std::string zmq_address, channel_list;
    uint16_t port;
    uint32_t channel;
    int hwm;
    size_t num_requested_samples;
    double total_time;
    float amplitude, phase;

    // setup the program options
    po::options_description desc("Allowed options");
    // clang-format off

    desc.add_options()
        ("help", "help message")
        ("nsamps", po::value<size_t>(&num_requested_samples)->default_value(0), "total number of samples to receive")
        ("duration", po::value<double>(&total_time)->default_value(0), "total number of seconds to receive")
        ("amplitude", po::value<float>(&amplitude)->default_value(1), "amplitude correction of second channel")
        ("phase", po::value<float>(&phase)->default_value(0), "phase shift on second channel [0-1]")
        ("progress", "periodically display short-term bandwidth")
        ("channel", po::value<std::string>(&channel_list)->default_value("0"), "which VRT channel(s) to use (specify \"0\", \"1\", \"0,1\", etc)")
        ("int-second", "align start of reception to integer second")
        ("null", "run without writing to file")
        ("continue", "don't abort on a bad packet")
        ("address", po::value<std::string>(&zmq_address)->default_value("localhost"), "DIFI ZMQ address")
        ("port", po::value<uint16_t>(&port)->default_value(50100), "DIFI ZMQ port")
        ("hwm", po::value<int>(&hwm)->default_value(10000), "DIFI ZMQ HWM")
    ;
    // clang-format on
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // print the help message
    if (vm.count("help")) {
        std::cout << boost::format("DIFI samples to nothing. %s") % desc << std::endl;
        std::cout << std::endl
                  << "This application streams data from a DIFI stream "
                     "to nowhwere.\n"
                  << std::endl;
        return ~0;
    }

    bool progress               = vm.count("progress") > 0;
    bool stats                  = vm.count("stats") > 0;
    bool null                   = vm.count("null") > 0;
    bool continue_on_bad_packet = vm.count("continue") > 0;
    bool int_second             = (bool)vm.count("int-second");

    if (!vm.count("int-second")) throw std::runtime_error("Dada requires --int-second");

    std::complex<float> z(0,-2*(float)M_PI*phase);
    std::complex<float> a(amplitude,0);
    std::complex<float> correction = a*exp(z);

    context_type difi_context;
    init_context(&difi_context);

    difi_packet_type difi_packet;

    difi_packet.channel_filt = 0;

     // detect which channels to use
    std::vector<std::string> channel_strings;
    std::vector<size_t> channel_nums;
    boost::split(channel_strings, channel_list, boost::is_any_of("\"',"));
    for (size_t ch = 0; ch < channel_strings.size(); ch++) {
        size_t chan = std::stoi(channel_strings[ch]);
        channel_nums.push_back(std::stoi(channel_strings[ch]));
        difi_packet.channel_filt |= 1<<std::stoi(channel_strings[ch]);
    }

    // DADA
    dada_hdu_t *dada_hdu;
    multilog_t *dada_log;
    std::string dada_header;
    std::complex<float> dadabuffer[DIFI_SAMPLES_PER_PACKET*MAX_CHANNELS] __attribute((aligned(32)));

    // ZMQ
    void *context = zmq_ctx_new();
    void *subscriber = zmq_socket(context, ZMQ_SUB);
    int rc = zmq_setsockopt (subscriber, ZMQ_RCVHWM, &hwm, sizeof hwm);
    std::string connect_string = "tcp://" + zmq_address + ":" + std::to_string(port);
    rc = zmq_connect(subscriber, connect_string.c_str());
    assert(rc == 0);
    zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0);

    // time keeping
    auto start_time = std::chrono::steady_clock::now();
    auto stop_time = start_time + std::chrono::milliseconds(int64_t(1000 * total_time));

    uint32_t buffer[ZMQ_BUFFER_SIZE];
    
    unsigned long long num_total_samps = 0;

    // Track time and samps between updating the BW summary
    auto last_update                     = start_time;
    unsigned long long last_update_samps = 0;

    bool first_frame = true;
    uint64_t last_fractional_seconds_timestamp = 0;

    // set to true to process data before context
    bool start_rx = false;

    uint32_t signal_pointer = 0;

    while (not stop_signal_called
           and (num_requested_samples*channel_nums.size() > num_total_samps or num_requested_samples == 0)) {

        int len = zmq_recv(subscriber, buffer, ZMQ_BUFFER_SIZE, 0);

        const auto now = std::chrono::steady_clock::now();

        if (not difi_process(buffer, sizeof(buffer), &difi_context, &difi_packet)) {
            printf("Not a Vita49 packet?\n");
            continue;
        }

        uint32_t ch = 0;
        while(not (difi_packet.stream_id & (1 << channel_nums[ch]) ) )
            ch++;

        uint32_t channel = channel_nums[ch];

        if (not start_rx and difi_packet.context) {
            difi_print_context(&difi_context);
            start_rx = true;

            if (total_time > 0)  
                num_requested_samples = total_time * difi_context.sample_rate;

            // Possibly do something with context here
            // DADA
            dada_header = 
              "HEADER DADA\n"
              "HDR_VERSION 1.0\n"
              "HDR_SIZE    4096\n"
              "FREQ " + std::to_string(difi_context.rf_freq/1e6) + "\n"
              "BW " + std::to_string(int(difi_context.sample_rate/1e6)) + "\n"
              "TELESCOPE DWL\n"
              "RECEIVER VRT\n"
              "INSTRUMENT dspsr\n"
              "SOURCE UNDEFINED\n"
              "NBIT " + "32\n" +
              "NDIM " + "2\n" +
              "NPOL " + std::to_string(channel_nums.size()) + "\n" +
              "RESOLUTION 1\n"
              "OBS_OFFSET 0\n"
              "TSAMP " + std::to_string(1e6/difi_context.sample_rate) + "\n";

            // DADA hdu
            dada_log = multilog_open ("example_dada_writer", 0);
            multilog_add(dada_log, stderr);
            dada_hdu = dada_hdu_create(dada_log);
            dada_hdu_set_key(dada_hdu, 0xc2c2);
            if (dada_hdu_connect (dada_hdu) < 0)
                throw std::runtime_error("Could not connect to DADA HDU");
            if (dada_hdu_lock_write(dada_hdu) < 0 )
                throw std::runtime_error("Could not get write lock on DADA HDU");
            // END DADA
        }
        
        if (start_rx and difi_packet.data) {

            if (difi_packet.lost_frame)
               if (not continue_on_bad_packet)
                    break;

            if (int_second) {
                // check if fractional second has wrapped
                if (difi_packet.fractional_seconds_timestamp > last_fractional_seconds_timestamp ) {
                        last_fractional_seconds_timestamp = difi_packet.fractional_seconds_timestamp;
                        continue;
                } else {
                    int_second = false;
                    last_update = now; 
                    start_time = now;
                    stop_time = start_time + std::chrono::milliseconds(int64_t(1000 * total_time));
                }
            }

            // Process data here
            // Assumes ci16_le

            for (uint32_t i = 0; i < difi_packet.num_rx_samps; i++) {
                int16_t re;
                memcpy(&re, (char*)&buffer[difi_packet.offset+i], 2);
                int16_t img;
                memcpy(&img, (char*)&buffer[difi_packet.offset+i]+2, 2);
                // Convert ci16_le to float
                std::complex<float>sample(re,img);
                if (channel_nums.size() > 1) {
                    if (ch==1)
                        dadabuffer[i*channel_nums.size()+ch] = correction*sample;
                    else 
                        dadabuffer[i*channel_nums.size()+ch] = sample;
                } else {
                    dadabuffer[i] = sample; 
                }
            }

            // send when all channels have been received
            if (ch == channel_nums.size()-1) {
                if (ipcio_write(dada_hdu->data_block, (char*)dadabuffer, channel_nums.size()*difi_packet.num_rx_samps*sizeof(std::complex<float>)) < 0)
                    throw std::runtime_error("Error writing buffer to DADA");
            }

            // data: (const char*)&buffer[difi_packet.offset]
            // size (bytes): sizeof(uint32_t)*difi_packet.num_rx_samps
             
            num_total_samps += difi_packet.num_rx_samps;

            if (start_rx and first_frame) {
                std::cout << boost::format(
                                 "  First frame: %u samples, %u full secs, %.09f frac secs")
                                 % difi_packet.num_rx_samps
                                 % difi_packet.integer_seconds_timestamp
                                 % ((double)difi_packet.fractional_seconds_timestamp/1e12)
                          << std::endl;
                first_frame = false;
                // DADA
                // Put starttime into dada_header
                std::ostringstream starttime_str;
                std::time_t starttime_time_t = difi_packet.integer_seconds_timestamp;
                std::tm *starttime_tm = std::gmtime(&starttime_time_t);
                starttime_str << std::put_time(starttime_tm, "%Y-%m-%d-%H:%M:%S");
                dada_header.append("UTC_START " + starttime_str.str() + "\n");
                dada_header.resize(4096, ' ');
                // Write dada header to dada_header.txt for debugging
                std::ofstream dada_header_txt("dada_header.txt");
                dada_header_txt << dada_header;
                dada_header_txt.close();
                {
                   // Write dada header to buffer
                   uint64_t header_size = ipcbuf_get_bufsz (dada_hdu->header_block);
                   char * ipc_header = ipcbuf_get_next_write (dada_hdu->header_block);
                   strncpy(ipc_header, dada_header.c_str(), header_size);

                   if (ipcbuf_mark_filled (dada_hdu->header_block, 4096) < 0)
                       throw std::runtime_error("Could not mark filled Header Block");
                }
            }
        }

        if (progress) {
            if (difi_packet.data)
                last_update_samps += difi_packet.num_rx_samps;
            const auto time_since_last_update = now - last_update;
            if (time_since_last_update > std::chrono::seconds(1)) {
                const double time_since_last_update_s =
                    std::chrono::duration<double>(time_since_last_update).count();
                const double rate = double(last_update_samps) / time_since_last_update_s;
                std::cout << "\t" << (rate / 1e6) << " Msps, ";
                
                last_update_samps = 0;
                last_update       = now;
    
                float sum_i = 0;
                uint32_t clip_i = 0;

                double datatype_max = 32768.;

                for (int i=0; i<difi_packet.num_rx_samps; i++ ) {
                    auto sample_i = get_abs_val((std::complex<int16_t>)buffer[difi_packet.offset+i]);
                    sum_i += sample_i;
                    if (sample_i > datatype_max*0.99)
                        clip_i++;
                }
                sum_i = sum_i/difi_packet.num_rx_samps;
                std::cout << boost::format("%.0f") % (100.0*log2(sum_i)/log2(datatype_max)) << "% I (";
                std::cout << boost::format("%.0f") % ceil(log2(sum_i)+1) << " of ";
                std::cout << (int)ceil(log2(datatype_max)+1) << " bits), ";
                std::cout << "" << boost::format("%.0f") % (100.0*clip_i/difi_packet.num_rx_samps) << "% I clip, ";
                std::cout << std::endl;

            }
        }
    }

    // DADA
    // unlock write access from the HDU, performs implicit EOD
    if (dada_hdu_unlock_write (dada_hdu) < 0)
        throw std::runtime_error("dada_hdu_unlock_write failed");

    // disconnect from HDU
    if (dada_hdu_disconnect (dada_hdu) < 0)
        throw std::runtime_error("could not unlock write on DADA hdu");

    zmq_close(subscriber);
    zmq_ctx_destroy(context);

    return 0;

}  
