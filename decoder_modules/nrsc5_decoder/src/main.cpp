#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/multirate/power_decimator.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cstdio>
// nrsc5.h declares a plain C API with no extern "C" guard of its own, so wrap it
// to avoid C++ name mangling of the nrsc5_* symbols (otherwise they fail to resolve
// against libnrsc5 at dlopen time).
extern "C" {
#include <nrsc5.h>
}

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// We run the VFO at nrsc5's native CU8 rate (1488375 Hz) and decimate by 2 to reach the
// cs16 FM pipe rate (744187.5 Hz). This is deliberate: 744187.5 is fractional, and the VFO's
// RationalResampler rounds its target to an integer, turning the otherwise-clean ratio into
// a near-coprime one (interp ~186047) that designs a ~19M-tap filter. Targeting the integer
// 1488375 instead keeps the VFO resampler sane, and an exact power-of-two decimation gets us
// to 744187.5 cheaply (a few-tap halfband) with no rounding error.
#define VFO_SAMPLE_RATE     NRSC5_SAMPLE_RATE_CU8        // 1488375 Hz (integer)
#define NRSC5_DECIM         2                            // 1488375 / 2 = 744187.5 = cs16 FM rate
// Occupied bandwidth of a hybrid FM IBOC signal (analog + digital sidebands near +/-198 kHz).
#define VFO_BANDWIDTH       400000.0
// HD Radio supports up to 8 audio programs (0..7); HD1..HD4 are the common ones.
#define NRSC5_MAX_PROGRAMS  8

SDRPP_MOD_INFO{
    /* Name:            */ "nrsc5_decoder",
    /* Description:     */ "HD Radio (NRSC-5) decoder for SDR++",
    /* Author:          */ "Semper-Viventem",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

class NRSC5DecoderModule : public ModuleManager::Instance {
public:
    NRSC5DecoderModule(std::string name) {
        this->name = name;

        // Load config
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["program"] = 0;
        }
        int prog = config.conf[name]["program"];
        if (prog < 0 || prog >= NRSC5_MAX_PROGRAMS) { prog = 0; }
        program = prog;
        config.release(true);

        // Conversion buffer for IQ (interleaved int16: 2 values per complex sample)
        iqBuf.resize(STREAM_BUFFER_SIZE * 2);

        // Open the nrsc5 decoder in pipe mode. We feed it IQ samples; it demodulates
        // synchronously inside nrsc5_pipe_samples_cs16() and reports audio + metadata
        // through nrsc5Callback() on our IQ pump thread (no nrsc5_start() in pipe mode).
        if (nrsc5_open_pipe(&radio) != 0) {
            flog::error("nrsc5_decoder: failed to open nrsc5 pipe decoder");
            radio = NULL;
        }
        else {
            nrsc5_set_mode(radio, NRSC5_MODE_FM);
            nrsc5_set_callback(radio, nrsc5Callback, this);
        }

        // VFO locked to nrsc5's required input rate
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, VFO_BANDWIDTH, VFO_SAMPLE_RATE, VFO_BANDWIDTH, VFO_BANDWIDTH, true);

        // IQ pump: VFO output (1488375 Hz) -> decimate by 2 (744187.5 Hz) -> nrsc5
        decim.init(vfo->output, NRSC5_DECIM);
        iqSink.init(&decim.out, iqHandler, this);

        // Audio: nrsc5 emits 44100 Hz stereo, resample to the sink's rate
        resamp.init(&audioStream, NRSC5_SAMPLE_RATE_AUDIO, audioSampRate);
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(&resamp.out, &srChangeHandler, audioSampRate);
        sigpath::sinkManager.registerStream(name, &stream);

        decim.start();
        iqSink.start();
        resamp.start();
        stream.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~NRSC5DecoderModule() {
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled) {
            // Unblock any audio swap in flight before stopping the pump (see disable()).
            audioStream.stopWriter();
            iqSink.stop();
            decim.stop();
            resamp.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        sigpath::sinkManager.unregisterStream(name);
        if (radio) { nrsc5_close(radio); radio = NULL; }
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, std::clamp<double>(0, -bw / 2.0, bw / 2.0), VFO_BANDWIDTH, VFO_SAMPLE_RATE, VFO_BANDWIDTH, VFO_BANDWIDTH, true);

        decim.setInput(vfo->output);

        audioStream.clearWriteStop();
        decim.start();
        iqSink.start();
        resamp.start();
        enabled = true;
    }

    void disable() {
        // The IQ pump thread can be parked inside nrsc5Callback() -> handleAudio() ->
        // audioStream.swap(), which only unblocks via stopWriter() (audioStream is not a
        // registered output of iqSink, so iqSink.stop() alone would deadlock on join).
        audioStream.stopWriter();
        iqSink.stop();
        decim.stop();
        resamp.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        NRSC5DecoderModule* _this = (NRSC5DecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // Program selector (HD1..HD4)
        char current[16];
        snprintf(current, sizeof(current), "HD%d", _this->program + 1);
        ImGui::TextUnformatted("Program");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::BeginCombo(CONCAT("##nrsc5_prog_", _this->name), current)) {
            for (int i = 0; i < 4; i++) {
                char label[16];
                snprintf(label, sizeof(label), "HD%d", i + 1);
                bool avail = _this->programAvailable[i];
                if (!avail) { style::beginDisabled(); }
                if (ImGui::Selectable(label, _this->program == i)) {
                    _this->program = i;
                    config.acquire();
                    config.conf[_this->name]["program"] = i;
                    config.release(true);
                }
                if (!avail) { style::endDisabled(); }
            }
            ImGui::EndCombo();
        }

        // Status
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->synced) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Synchronized");
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Acquiring...");
        }
        ImGui::Text("MER: %.1f / %.1f dB", _this->merLower, _this->merUpper);
        ImGui::Text("BER: %.4f", _this->ber);

        // Station / now-playing metadata for the selected program
        {
            std::lock_guard<std::mutex> lck(_this->metaMtx);
            int p = _this->program;
            ImGui::BeginTable(CONCAT("##nrsc5_info_", _this->name), 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders);

            drawInfoRow("Station", _this->stationName);
            drawInfoRow("Slogan", _this->stationSlogan);
            drawInfoRow("Title", _this->progTitle[p]);
            drawInfoRow("Artist", _this->progArtist[p]);
            drawInfoRow("Album", _this->progAlbum[p]);

            ImGui::EndTable();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    static void drawInfoRow(const char* label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(value.empty() ? "--" : value.c_str());
    }

    // === nrsc5 <-> SDR++ glue ===

    // IQ pump (runs on its own DSP thread): convert float IQ to interleaved int16 and
    // hand it to nrsc5, which demodulates synchronously and invokes nrsc5Callback().
    static void iqHandler(dsp::complex_t* data, int count, void* ctx) {
        NRSC5DecoderModule* _this = (NRSC5DecoderModule*)ctx;
        if (!_this->radio) { return; }
        int16_t* buf = _this->iqBuf.data();
        for (int i = 0; i < count; i++) {
            float re = std::clamp(data[i].re * 32767.0f, -32767.0f, 32767.0f);
            float im = std::clamp(data[i].im * 32767.0f, -32767.0f, 32767.0f);
            buf[2 * i]     = (int16_t)re;
            buf[2 * i + 1] = (int16_t)im;
        }
        nrsc5_pipe_samples_cs16(_this->radio, buf, (unsigned int)(count * 2));
    }

    // Decoded audio for one program (44100 Hz, interleaved int16 L/R: count == frames*2).
    void handleAudio(unsigned int prog, const int16_t* data, size_t count) {
        if (prog < NRSC5_MAX_PROGRAMS) { programAvailable[prog] = true; }
        if ((int)prog != program) { return; }
        int frames = (int)(count / 2);
        dsp::stereo_t* out = audioStream.writeBuf;
        for (int i = 0; i < frames; i++) {
            out[i].l = (float)data[2 * i]     / 32768.0f;
            out[i].r = (float)data[2 * i + 1] / 32768.0f;
        }
        audioStream.swap(frames);
    }

    static void nrsc5Callback(const nrsc5_event_t* evt, void* opaque) {
        NRSC5DecoderModule* _this = (NRSC5DecoderModule*)opaque;
        switch (evt->event) {
            case NRSC5_EVENT_SYNC:
                _this->synced = true;
                break;
            case NRSC5_EVENT_LOST_SYNC:
                _this->synced = false;
                break;
            case NRSC5_EVENT_MER:
                _this->merLower = evt->mer.lower;
                _this->merUpper = evt->mer.upper;
                break;
            case NRSC5_EVENT_BER:
                _this->ber = evt->ber.cber;
                break;
            case NRSC5_EVENT_AUDIO:
                _this->handleAudio(evt->audio.program, evt->audio.data, evt->audio.count);
                break;
            case NRSC5_EVENT_AUDIO_SERVICE:
                if (evt->audio_service.program < NRSC5_MAX_PROGRAMS) {
                    _this->programAvailable[evt->audio_service.program] = true;
                }
                break;
            case NRSC5_EVENT_ID3: {
                unsigned int p = evt->id3.program;
                if (p < NRSC5_MAX_PROGRAMS) {
                    std::lock_guard<std::mutex> lck(_this->metaMtx);
                    _this->progTitle[p]  = evt->id3.title  ? evt->id3.title  : "";
                    _this->progArtist[p] = evt->id3.artist ? evt->id3.artist : "";
                    _this->progAlbum[p]  = evt->id3.album  ? evt->id3.album  : "";
                }
                break;
            }
            case NRSC5_EVENT_STATION_NAME: {
                std::lock_guard<std::mutex> lck(_this->metaMtx);
                _this->stationName = evt->station_name.name ? evt->station_name.name : "";
                break;
            }
            case NRSC5_EVENT_STATION_SLOGAN: {
                std::lock_guard<std::mutex> lck(_this->metaMtx);
                _this->stationSlogan = evt->station_slogan.slogan ? evt->station_slogan.slogan : "";
                break;
            }
            default:
                break;
        }
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        NRSC5DecoderModule* _this = (NRSC5DecoderModule*)ctx;
        _this->audioSampRate = sampleRate;
        _this->resamp.tempStop();
        _this->resamp.setOutSamplerate(sampleRate);
        _this->resamp.tempStart();
    }

    std::string name;
    bool enabled = true;

    nrsc5_t* radio = NULL;
    std::vector<int16_t> iqBuf;

    VFOManager::VFO* vfo = NULL;
    dsp::multirate::PowerDecimator<dsp::complex_t> decim;
    dsp::sink::Handler<dsp::complex_t> iqSink;

    // Manually-driven audio stream written from nrsc5Callback() -> handleAudio()
    dsp::stream<dsp::stereo_t> audioStream;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;

    double audioSampRate = 48000;
    EventHandler<float> srChangeHandler;
    SinkManager::Stream stream;

    // Selected audio program (0 == HD1). Written by the GUI thread, read by the pump
    // thread in handleAudio(), so use an atomic to avoid a formal data race.
    std::atomic<int> program = 0;

    // Status (written on the callback thread, read on the GUI thread; benign races)
    bool synced = false;
    float merLower = 0.0f;
    float merUpper = 0.0f;
    float ber = 0.0f;
    bool programAvailable[NRSC5_MAX_PROGRAMS] = { false };

    // Metadata (guarded by metaMtx)
    std::mutex metaMtx;
    std::string stationName;
    std::string stationSlogan;
    std::string progTitle[NRSC5_MAX_PROGRAMS];
    std::string progArtist[NRSC5_MAX_PROGRAMS];
    std::string progAlbum[NRSC5_MAX_PROGRAMS];
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/nrsc5_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new NRSC5DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (NRSC5DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
