#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <iostream>
#include <tuple>

const uint8_t OBJECT_CODE_PPS_SECOND = 0xe0;
const uint8_t OBJECT_CODE_PPS_YEAR = 0xe4;
const uint8_t OBJECT_CODE_TRIG_CONFIG = 0xe5;
const uint8_t OBJECT_CODE_DATA_FORMAT = 0xe6;
const uint16_t STATUS_CPUTRIG_ACTIVE = (1 << 5);

class FramePpsYear {
   public:
    unsigned int year;
};

class FramePpsSecond {
   public:
    unsigned int second;
};

// always assume 3 adcs for now
class FrameHit {
   public:
    std::tuple<uint16_t, uint16_t, uint16_t> adcs;
    bool cpu_trigger;
};

class HitBufferDecoder {
    pybind11::buffer input;
    size_t position;
    bool cpu_trigger;

   public:
    HitBufferDecoder(pybind11::buffer _input) {
        input = _input;
        // initialize
        position = 0;
        cpu_trigger = false;
        // check buffer valid
        auto info = input.request();
        if (info.size % 4 != 0) {
            throw pybind11::value_error("input must be multiple of four bytes");
        }
        input.inc_ref();
    }

    ~HitBufferDecoder() { this->input.dec_ref(); }

    pybind11::object __next__() {
        auto info = input.request();
        size_t length = (size_t)(info.size / 4);
        uint32_t *data = (uint32_t *)info.ptr;
        // search for next frame
        while (this->position < length) {
            uint32_t header = data[this->position++];
            uint8_t frame_type = header >> 24;
            switch (frame_type) {
                case OBJECT_CODE_PPS_SECOND: {
                    auto second = new FramePpsSecond();
                    second->second = header & ((1 << 24) - 1);
                    return pybind11::cast(second);
                }
                case OBJECT_CODE_PPS_YEAR: {
                    auto year = new FramePpsYear();
                    year->year = header & ((1 << 16) - 1);
                    return pybind11::cast(year);
                }
                case OBJECT_CODE_TRIG_CONFIG: {
                    // no idea what this is used for
                    uint8_t trigger_mode = (header >> 16) & 0xff;
                    uint16_t rc_status = header & 0xffff;
                    // check enough data remaining
                    if (this->position >= length) {
                        throw pybind11::value_error(
                            "incomplete frame at end of hitbuffer file");
                    }
                    uint32_t offset = data[this->position++];
                    this->cpu_trigger =
                        (rc_status & STATUS_CPUTRIG_ACTIVE) != 0;
                } break;
                case OBJECT_CODE_DATA_FORMAT: {
                    // do nothing for now
                } break;
                default: {
                    // hit
                    uint32_t offset = header;
                    // get "multi" word
                    if (this->position >= length) {
                        throw pybind11::value_error(
                            "incomplete frame at end of hitbuffer file");
                    }
                    uint32_t multi = data[this->position++];
                    // decode "multi" word
                    uint8_t adc_count = (multi >> 28) & 0xf;
                    uint16_t tot = (multi >> 16) & 0xfff;
                    if (adc_count != 3) {
                        throw pybind11::value_error("invalid number of ADCs");
                    }
                    // construct hit
                    auto hit = new FrameHit();
                    hit->cpu_trigger = this->cpu_trigger;
                    std::get<0>(hit->adcs) = multi & 0x0fff;
                    // get adc word
                    if (this->position >= length) {
                        throw pybind11::value_error(
                            "incomplete frame at end of hitbuffer file");
                    }
                    uint32_t adcs = data[this->position++];
                    std::get<1>(hit->adcs) = (adcs >> 16) & 0x0fff;
                    std::get<2>(hit->adcs) = adcs & 0x0fff;
                    return pybind11::cast(hit);
                }
            }
        }
        throw pybind11::stop_iteration();
    }

    HitBufferDecoder *__iter__() { return this; }
};

// check if there are count elements remaining and subtract count from length
void take_from_length(size_t &length, size_t count = 1) {
    if (length < count) {
        throw pybind11::value_error(
            "incomplete frame at end of hitbuffer file");
    }
    length -= count;
}

// average all cpu triggers, return hits and counts
constexpr size_t N_ADC = 3;

auto get_baseline(pybind11::buffer input, uint16_t adc_clip) {
    static_assert(N_ADC >= 2 && N_ADC <= 3,
                  "only 2 and 3 ADCs are supported currently");
    // check input is valid
    auto info = input.request();
    if (info.size % 4 != 0) {
        throw pybind11::value_error("input must be multiple of four bytes");
    }
    // get data
    size_t length = (size_t)(info.size / 4);
    uint32_t *data = (uint32_t *)info.ptr;
    // keep track of hits
    std::array<uint64_t, N_ADC> baseline = {0};
    std::array<uint64_t, N_ADC> count = {0};
    bool cpu_trigger = false;
    // loop over data
    while (length) {
        length--;
        uint32_t header = *data++;
        uint8_t frame_type = header >> 24;
        switch (frame_type) {
            // ignore PPS and data format
            case OBJECT_CODE_PPS_SECOND:
            case OBJECT_CODE_PPS_YEAR:
            case OBJECT_CODE_DATA_FORMAT:
                break;
            // get cpu_trigger status
            case OBJECT_CODE_TRIG_CONFIG: {
                // get cpu trigger flag
                uint16_t rc_status = header & 0xffff;
                cpu_trigger = (rc_status & STATUS_CPUTRIG_ACTIVE) != 0;
                // consume additional word (offset)
                take_from_length(length);
                data++;
            } break;
            /* HIT */ default : {
                // header contains offset, unused
                // check for two additional words
                take_from_length(length, 2);
                // get additional words
                uint32_t multi = *data++;
                uint32_t adc_word = *data++;
                // decode "multi" word
                uint8_t adc_count = (multi >> 28) & 0xf;
                if (adc_count != N_ADC) {
                    throw pybind11::value_error("invalid number of ADCs");
                }
                // use only cpu triggers
                if (!cpu_trigger) {
                    continue;
                }
                // high gain
                uint16_t adc = multi & 0x0fff;
                if (adc <= adc_clip) {
                    baseline[0] += adc;
                    count[0]++;
                }
                // medium gain
                adc = (adc_word >> 16) & 0x0fff;
                if (adc <= adc_clip) {
                    baseline[1] += adc;
                    count[1]++;
                }
                // low gain
                if (N_ADC > 2) {
                    adc = adc_word & 0x0fff;
                    if (adc <= adc_clip) {
                        baseline[2] += adc;
                        count[2]++;
                    }
                }
            }
        }
    }
    return std::make_tuple(std::tuple_cat(baseline), std::tuple_cat(count));
}

// works only with 3 active adcs rn
std::tuple<uint64_t, uint64_t> get_hitrate_thresh(
    pybind11::buffer input,
    std::array<double, 2> adc_amp, // amplification of adcs 1 and 2, relative to adc 0 (usually smaller than zero)
    std::array<double, 3> baseline_adc, // ADC baseline "pedestal"
    double mip_per_adc0,
    double threshold_mip,
    uint16_t max_adc_counts // max ADC count before switching to next ADC
) {
    // check input is valid
    auto info = input.request();
    if (info.size % 4 != 0) {
        throw pybind11::value_error("input must be multiple of four bytes");
    }
    // get data
    size_t length = (size_t)(info.size / 4);
    uint32_t *data = (uint32_t *)info.ptr;
    // prepare counters
    uint64_t seconds = 0, hits = 0, hits_temp = 0;
    bool cpu_trigger = false;
    // loop over all data
    while (length) {
        length--;
        uint32_t header = *data++;
        uint8_t frame_type = header >> 24;
        switch (frame_type) {
            // ignore PPS year and data format
            case OBJECT_CODE_PPS_YEAR:
            case OBJECT_CODE_DATA_FORMAT:
                break;
            // start counting hits only after two seconds have elapsed (the
            // first "seconds" frame is not at a full second)
            case OBJECT_CODE_PPS_SECOND: {
                if (seconds < 2) {
                    hits_temp = 0;
                }
                hits = hits_temp;
                seconds++;
                break;
            }
            // get cpu_trigger status
            case OBJECT_CODE_TRIG_CONFIG: {
                // get cpu trigger flag
                uint16_t rc_status = header & 0xffff;
                cpu_trigger = (rc_status & STATUS_CPUTRIG_ACTIVE) != 0;
                // consume additional word (offset)
                take_from_length(length);
                data++;
            } break;
            /* HIT */ default : {
                // header contains offset, unused
                // check for two additional words
                take_from_length(length, 2);
                // get additional words
                uint32_t multi = *data++;
                uint32_t adc_word = *data++;
                // decode "multi" word
                uint8_t adc_count = (multi >> 28) & 0xf;
                if (adc_count != 3) {
                    throw pybind11::value_error("invalid number of ADCs");
                }
                // use only actual trigger signals
                if (cpu_trigger) {
                    continue;
                }
                // extract adc counts
                std::array<uint16_t, 3> adcs;
                adcs[0] = multi & 0x0fff; // high gain
                adcs[1] = (adc_word >> 16) & 0x0fff; // medium gain
                adcs[2] = adc_word & 0x0fff; // low gain
                // find suitable adc
                size_t adc_index = 0;
                while (adc_index < adcs.size() && adcs[adc_index] > max_adc_counts) {
                    adc_index++;
                }
                // all adcs saturated?
                if (adc_index >= adcs.size()) {
                    std::cerr << "all ADCs are saturated" << std::endl;
                    continue;
                }
                // convert to MIPS
                double mips = adcs[adc_index] - baseline_adc[adc_index];
                mips *= mip_per_adc0;
                // apply adc gain
                if (adc_index > 0) {
                    mips *= adc_amp[adc_index - 1];
                }
                // check threshold
                if (mips < threshold_mip) {
                    continue;
                }
                hits_temp++;
            }
        }
    }
    return std::make_tuple(seconds, hits);
}

std::tuple<int, int> analyze_hitbuf(pybind11::buffer input) {
    pybind11::buffer_info info(input.request());
    int seconds = 0, hits = 0, hits_temp = 0;
    // all byte objects seem to be aligned to 16 bits
    uint32_t *data = (uint32_t *)info.ptr;
    std::size_t count = info.size / sizeof(uint32_t);
    for (size_t i = 0; i < count; i++) {
        uint32_t header = data[i];
        uint8_t type = static_cast<uint8_t>(header >> 24);
        switch (type) {
            case OBJECT_CODE_PPS_SECOND: {
                if (seconds < 2) {
                    hits_temp = 0;
                }
                hits = hits_temp;
                seconds++;
                break;
            }
            case OBJECT_CODE_TRIG_CONFIG: {
                i++;  // skip one word
                if (i > count) {
                    throw pybind11::value_error("incomplete frame");
                }
                break;
            }
            case OBJECT_CODE_PPS_YEAR:
            case OBJECT_CODE_DATA_FORMAT: {
                break;  // do nothing
            }
            default: {
                hits_temp++;
                i++;  // next word contains number of ADCs
                if (i >= count) {
                    throw pybind11::value_error("incomplete frame");
                }
                std::size_t adc_count = (data[i] >> 28) & 0xf;
                std::size_t extra_words = adc_count / 2;
                i += extra_words;
                if (i > count) {
                    throw pybind11::value_error("incomplete frame");
                }
                break;
            }
        }
    }
    seconds -= 2;
    if (seconds < 1) {
        throw pybind11::value_error("too little data");
    }
    return std::make_tuple(seconds, hits);
}

PYBIND11_MODULE(analyze_hitbuffer, m) {
    m.doc() = "pybind11 example plugin";  // optional module docstring

    m.def("get_baseline", &get_baseline, "get_baseline");

    m.def("get_hitrate_thresh", &get_hitrate_thresh, "get_hitrate_thresh");
    m.def("analyze_hitbuf", &analyze_hitbuf, "analyze_hitbuf");

    pybind11::class_<HitBufferDecoder>(m, "HitBufferDecoder")
        .def(pybind11::init<pybind11::buffer>())
        .def("__next__", &HitBufferDecoder::__next__)
        .def("__iter__", &HitBufferDecoder::__iter__);

    pybind11::class_<FramePpsSecond>(m, "FramePpsSecond")
        .def(pybind11::init<>())
        .def_readwrite("second", &FramePpsSecond::second);

    pybind11::class_<FramePpsYear>(m, "FramePpsYear")
        .def(pybind11::init<>())
        .def_readwrite("year", &FramePpsYear::year);

    pybind11::class_<FrameHit>(m, "FrameHit")
        .def(pybind11::init<>())
        .def_readwrite("adcs", &FrameHit::adcs)
        .def_readwrite("cpu_trigger", &FrameHit::cpu_trigger);
}
