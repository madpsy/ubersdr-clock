// ubersdr-clock — standalone WWV/WWVH/WWVB/DCF77/MSF/Allouis time-code decoder.
//
// Reads raw int16 little-endian PCM on stdin — mono audio for WWV/WWVH/WWVB,
// interleaved I/Q for DCF77, MSF and Allouis — and writes newline-delimited JSON events on
// stdout. Same shape as UberSDR's other external decoder
// binaries (cw-decoder, ubersdr-drm, freedv-ka9q): no framing, no handshake,
// close stdin to stop.
//
// The DSP is AetherSDR's AetherClock chain as corrected by ubersdr-ntp, which
// is where it is now maintained — see PROVENANCE in the README. This file is
// the only part written for UberSDR: it replaces
// the Qt engine (AetherClockEngine) that fed the decoders there, and it does
// only what that engine did on the receive side — hold the sample<->host
// anchor, compose a UTC timestamp from the voted frame, and report the offset.
//
// Timing anchor
// ─────────────
// A pipe carries samples, not timestamps, so the host anchor here is simply
// the wall clock read when the first sample arrives, advanced by the sample
// count. That is exactly what AetherClockEngine does, and it is good to a few
// tens of ms — dominated by the buffering between the receiver and this
// process, which this process cannot see.
//
// UberSDR can do better, and should: every AudioSample reaching an audio
// extension carries a GPS-synchronised GPSTimeNs. Every event below therefore
// reports the raw sample indices (`edge_sample`, `frame_start_sample`,
// `last_edge_sample`) the offset was derived from, so the Go wrapper can
// recompute it against the GPS timestamps and ignore `offset_ms` entirely.
// `offset_ms` is for standalone use.

#include "WwvDecoder.h"
#include "WwvbDecoder.h"
#include "Dcf77Decoder.h"
#include "MsfDecoder.h"
#include "AllouisDecoder.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kVersion = "1.3.0";

// ---------------------------------------------------------------------------
// Calendar arithmetic (Howard Hinnant's civil-date algorithms).
//
// Deliberately not timegm()/gmtime_r(): both are POSIX rather than ISO C, and
// whether they are visible under -std=c++20 depends on feature-test macros. A
// decoder whose timestamps depend on how the compiler was invoked is not one
// to debug at 3am. These are exact for the whole proleptic Gregorian range.

long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);             // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;         // [0, 146096]
    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void civilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);          // [0, 146096]
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const long long yy = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);       // [0, 365]
    const unsigned mp = (5u * doy + 2u) / 153u;                            // [0, 11]
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp + (mp < 10u ? 3u : -9u);
    y = static_cast<int>(yy + (m <= 2u));
}

// Floor division/modulo — plain / and % truncate toward zero, which is wrong
// for pre-1970 epochs. Not reachable from a valid decode, but the fallback
// paths below hand these whatever the voter produced.
long long floorDiv(long long a, long long b) {
    const long long q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
long long floorMod(long long a, long long b) { return a - floorDiv(a, b) * b; }

// UTC milliseconds for a decoded WWV timestamp. doy is 1-based (NIST BCD day
// field), year2 the two-digit year in the 20xx century, seconds always 0 —
// a time-code frame names the minute, and the second comes from the edge.
long long utcMsFromFields(int year2, int doy, int hour, int minute) {
    const long long days = daysFromCivil(2000 + year2, 1, 1) + (doy - 1);
    return (days * 86400LL + hour * 3600LL + minute * 60LL) * 1000LL;
}

std::string iso8601(long long ms) {
    const long long secs = floorDiv(ms, 1000);
    const long long days = floorDiv(secs, 86400);
    long long rem = floorMod(secs, 86400);
    int y = 0; unsigned mo = 0, d = 0;
    civilFromDays(days, y, mo, d);
    const int hh = static_cast<int>(rem / 3600); rem %= 3600;
    const int mi = static_cast<int>(rem / 60);
    const int ss = static_cast<int>(rem % 60);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, mo, d, hh, mi, ss);
    return buf;
}

long long hostNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// The host clock as the voter's plausibility reference. Fires on this thread
// inside process(), so it must stay cheap — it is four integer divisions.
clockdec::TimeFields hostNowFields() {
    const long long secs = floorDiv(hostNowMs(), 1000);
    const long long days = floorDiv(secs, 86400);
    const long long rem = floorMod(secs, 86400);
    int y = 0; unsigned mo = 0, d = 0;
    civilFromDays(days, y, mo, d);
    clockdec::TimeFields tf;
    tf.minute = static_cast<int>((rem / 60) % 60);
    tf.hour = static_cast<int>(rem / 3600);
    tf.doy = static_cast<int>(days - daysFromCivil(y, 1, 1)) + 1;
    tf.year2 = y % 100;
    return tf;
}

// ---------------------------------------------------------------------------
// Minimal JSON line writer. One object per line, no nesting beyond a flat
// array, which is all any event below needs — so this stays a string builder
// rather than a dependency.

class Json {
public:
    Json() : m_s("{") {}

    Json& str(const char* k, std::string_view v) {
        key(k);
        m_s += '"';
        for (char c : v) {
            switch (c) {
                case '"':  m_s += "\\\""; break;
                case '\\': m_s += "\\\\"; break;
                case '\n': m_s += "\\n"; break;
                case '\r': m_s += "\\r"; break;
                case '\t': m_s += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char esc[8];
                        std::snprintf(esc, sizeof esc, "\\u%04x", c);
                        m_s += esc;
                    } else {
                        m_s += c;
                    }
            }
        }
        m_s += '"';
        return *this;
    }

    Json& i(const char* k, long long v) {
        key(k);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%lld", v);
        m_s += buf;
        return *this;
    }

    Json& b(const char* k, bool v) { key(k); m_s += v ? "true" : "false"; return *this; }

    // Non-finite becomes null: JSON has no NaN, and diagnostics() really does
    // report NaN for a delay estimate that has not settled.
    Json& f(const char* k, double v, int prec = 4) {
        key(k);
        m_s += fmt(v, prec);
        return *this;
    }

    Json& arr(const char* k, const std::vector<float>& v, int prec = 4) {
        key(k);
        m_s += '[';
        for (std::size_t n = 0; n < v.size(); ++n) {
            if (n) m_s += ',';
            m_s += fmt(v[n], prec);
        }
        m_s += ']';
        return *this;
    }

    // Writes the line and flushes. stdout on a pipe is fully buffered, so
    // without the flush the reading process sees nothing until the buffer
    // fills — minutes of silence for a decoder that emits one line a second.
    void emit() {
        m_s += "}\n";
        std::fwrite(m_s.data(), 1, m_s.size(), stdout);
        std::fflush(stdout);
    }

private:
    void key(const char* k) {
        if (!m_first) m_s += ',';
        m_first = false;
        m_s += '"';
        m_s += k;
        m_s += "\":";
    }

    static std::string fmt(double v, int prec) {
        if (!std::isfinite(v)) return "null";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", prec, v);
        return buf;
    }

    std::string m_s;
    bool m_first = true;
};

// ---------------------------------------------------------------------------

const char* stationName(clockdec::ClockStation s) {
    using clockdec::ClockStation;
    switch (s) {
        case ClockStation::Wwv:  return "wwv";
        case ClockStation::Wwvh: return "wwvh";
        case ClockStation::Wwvb: return "wwvb";
        case ClockStation::Dcf77: return "dcf77";
        case ClockStation::Msf:   return "msf";
        case ClockStation::Allouis: return "allouis";
        default:                 return "unknown";
    }
}

const char* stateName(clockdec::ClockLockState s) {
    using clockdec::ClockLockState;
    switch (s) {
        case ClockLockState::Locked:    return "locked";
        case ClockLockState::Acquiring: return "acquiring";
        default:                        return "nosignal";
    }
}

const char* refusalName(std::uint8_t r) {
    using clockdec::ClockLockRefusal;
    switch (static_cast<ClockLockRefusal>(r)) {
        case ClockLockRefusal::QualityFloor: return "quality_floor";
        case ClockLockRefusal::Plausibility: return "plausibility";
        case ClockLockRefusal::Staleness:    return "staleness";
        case ClockLockRefusal::Contested:    return "contested";
        default:                             return "none";
    }
}

// Where the WWV/WWVH decoder puts a second edge relative to the true edge in
// its input: 13.6 ms EARLY. The matched filter's chain group delay is taken as
// kNominalDelaySamples = 7 series samples where it measures 4.27, and one
// series sample at 200 Hz is 5 ms, so the label lands (7 - 4.27) * 5 ms early.
//
// Measured, not estimated: tools/decodertest synthesises WWV and WWVH and
// reports this mean over every edge it checks -- -13.642 and -13.635 ms, spread
// about +/-1 ms. The WWVB decoder's edges are exact to 0.02 ms, and DCF77's
// are exact to tools/dcf77test's resolution whichever of AM and PM is timing
// them, so neither has such a term.
//
// Corrected here rather than in the decoder. kNominalDelaySamples was
// calibrated upstream against real off-air audio through a real receive chain,
// and the delay tracker and its search rails are built around that value;
// moving it to suit a synthetic generator would be fitting the decoder to the
// test. The bias it leaves is a known constant, so it is taken off the offset
// instead. ubersdr-ntp carries the identical term for the identical reason.
constexpr double kWwvDecoderEdgeBiasMs = -13.645;

// Where the MSF decoder puts the second against where NPL does: 0.19 ms LATE,
// so +0.19 ms is added back. The decoder times the steepest point of the
// carrier's fall; NPL's second is the carrier going off, and the fall (shaped
// by Anthorn's antenna) takes a fraction of a millisecond to reach its
// steepest. Measured by ubersdr-ntp on M9PSY-1, capture-timed against a
// GPS-disciplined stratum 1, 2026-09-25 (night; to be confirmed by day) --
// its kMsfDecoderEdgeBiasSec. Allouis has none: its decoder reports the
// second itself, 50.48 ms after the phase excursion starts (measured the same
// way; AllouisDecoder.cpp).
constexpr double kMsfDecoderEdgeBiasMs = 0.19;

// Which decoder runs. WWV and WWVH are one decoder, which tags the station
// itself; the others are different signals altogether.
enum class Decoder { Wwv, Wwvb, Dcf77, Msf, Allouis };

// The decoders that read complex baseband: each tuned on its carrier, in IQ.
bool isIq(Decoder d) { return d == Decoder::Dcf77 || d == Decoder::Msf || d == Decoder::Allouis; }

// The carrier's frequency, for turning a dial frequency into where the carrier
// sits in the baseband.
constexpr double kDcf77CarrierHz = 77500.0;

struct Options {
    int sampleRate = 12000;
    // One-way delay this process cannot see, in milliseconds, added back to
    // offset_ms. See --extra-delay-ms in usage() and the delay section of the
    // README: for a receiver this is dominated by the ionospheric path from
    // the transmitter, which nothing here can compute.
    double extraDelayMs = 0.0;
    Decoder decoder = Decoder::Wwv;
    // IQ stations (DCF77, MSF, Allouis) only: where the carrier sits in the
    // complex baseband, in Hz. 0 when the dial is the carrier itself. The
    // decoder searches +/-20 Hz around this, so a dial a few Hz out costs nothing.
    double carrierOffsetHz = 0.0;
    bool seconds = true;
    bool envelope = false;
    int diagSeconds = 10;
    int plausibilityMinutes = 24 * 60;   // AetherClockEngine's kPlausibilityBoundMinutes
};

// Turns decoder callbacks into JSON lines, and holds the two pieces of state
// the decoders do not carry themselves: the sample<->host anchor, and the
// frame start the voted timestamp is composed against.
class Emitter {
public:
    Emitter(const Options& o, int sampleRate) : m_o(o), m_rate(sampleRate) {}

    void setAnchor(long long hostMs) { m_anchorMs = hostMs; m_haveAnchor = true; }

    double hostMsAtSample(std::int64_t n) const {
        return static_cast<double>(m_anchorMs) + 1000.0 * static_cast<double>(n) / m_rate;
    }

    void onState(clockdec::ClockLockState s, clockdec::ClockStation st) {
        Json j;
        j.str("type", "state").str("state", stateName(s)).str("station", stationName(st));
        j.emit();
    }

    void onSecond(const clockdec::ClockSecondInfo& i, clockdec::ClockStation st) {
        if (!m_o.seconds) return;
        Json j;
        j.str("type", "second")
         .i("edge_sample", i.edgeSample)
         // The same edge before it was rounded to a whole sample, where the
         // decoder resolves it finer (DCF77, MSF, Allouis); null where it does
         // not. A whole sample is 83 us at 12 kHz.
         .f("edge_sample_exact", i.edgeSampleExact, 3)
         .i("symbol", static_cast<long long>(i.symbol))
         .f("confidence", i.confidence)
         .i("second_of_frame", i.secondOfFrame)
         .i("series_rate", i.seriesRateHz)
         .i("window_shift", i.windowShift)
         // Whether THIS second carried its own timing evidence. False for
         // WWV/WWVH's minute hole (second 0 has no subcarrier pulse to align
         // to), for sub-threshold seconds, and for WWVB seconds whose carrier
         // drop was not found. The edge is still the decoder's tracked cadence
         // and is correct to its usual accuracy, but nothing in this second
         // measured it -- so a caller computing an offset from edge_sample
         // should prefer the seconds where this is true.
         .b("edge_measured", i.edgeMeasured)
         .str("station", stationName(st));
        if (m_o.envelope) {
            j.arr("envelope", i.envelope, 3);
            j.arr("expected", i.expected, 3);
        }
        j.emit();
    }

    void onFrame(const clockdec::ClockFrameInfo& f) {
        // Recorded whether or not the frame is emitted: onTime composes
        // against it, and a raw frame decode is never suppressed anyway.
        m_frameStartSample = f.frameStartSample;
        m_haveFrame = true;

        Json j;
        j.str("type", "frame")
         .i("minute", f.minute).i("hour", f.hour).i("doy", f.doy).i("year2", f.year2);
        if (m_o.decoder == Decoder::Dcf77 || m_o.decoder == Decoder::Allouis) {
            // DCF77 and Allouis send no DUT1, and a 0 here would read as "UT1
            // is in step with UTC" rather than "not sent". Their one zone bit
            // is CEST, which the decoder carries in both DST fields; named for
            // what it is.
            j.b("dst1", f.dst1).b("dst2", f.dst2).b("cest", f.dst1);
        } else if (m_o.decoder == Decoder::Msf) {
            // MSF sends DUT1, and UK clock time: its zone bit is BST.
            j.i("dut1_tenths", f.dut1Tenths).b("dst1", f.dst1).b("dst2", f.dst2).b("bst", f.dst1);
        } else {
            j.i("dut1_tenths", f.dut1Tenths).b("dst1", f.dst1).b("dst2", f.dst2);
        }
        j.b("leap_pending", f.leapPending).b("leap_year", f.leapYear)
         .f("confidence", f.frameConfidence)
         .i("frame_start_sample", f.frameStartSample)
         .str("station", stationName(f.station));
        j.emit();
    }

    void onTime(const clockdec::ClockTimeInfo& t) {
        // onFrame always precedes a vote, but a decoder that locked on a
        // backlog replay could in principle reach here first; composing
        // against a frame start of 0 would put the timestamp minutes out.
        if (!m_haveFrame || !m_haveAnchor) return;
        if (t.year2 < 0 || t.doy < 1 || t.hour < 0 || t.minute < 0) return;

        // Composed exactly as AetherClockEngine::handleTime does: the voted
        // frame's second 0, plus the elapsed samples to the last edge. Using
        // lastEdgeSecondOfFrame directly would be wrong — it can point into a
        // frame later than the one that was voted.
        const long long baseMs = utcMsFromFields(t.year2, t.doy, t.hour, t.minute);
        const long long elapsedSec = std::llround(
            static_cast<double>(t.lastEdgeSample - m_frameStartSample) / m_rate);
        const long long decodedMs = baseMs + elapsedSec * 1000LL;

        // The raw difference is the decoded time against the host clock at the
        // sample the edge was OBSERVED at, so it still contains every delay
        // between the transmitter and here. Two of those are known:
        //
        //   the decoder's own edge bias, which is this binary's business and is
        //   taken off unconditionally (WWV/WWVH only -- see the constant);
        //
        //   whatever the caller has measured or modelled and passed in, which
        //   for a receiver is mostly the ionospheric path. Added back, because
        //   the observation instant is late by it.
        //
        // What remains uncorrected is everything between the antenna and this
        // process's stdin: receiver buffering, the codec, the transport. A
        // caller that knows those should fold them into --extra-delay-ms.
        const double edgeBias = m_o.decoder == Decoder::Wwv ? kWwvDecoderEdgeBiasMs
                              : m_o.decoder == Decoder::Msf ? kMsfDecoderEdgeBiasMs : 0.0;
        const double offsetMs = static_cast<double>(decodedMs)
                                - hostMsAtSample(t.lastEdgeSample)
                                + edgeBias + m_o.extraDelayMs;

        Json j;
        j.str("type", "time")
         .str("utc", iso8601(decodedMs))
         .i("utc_ms", decodedMs)
         .i("minute", t.minute).i("hour", t.hour).i("doy", t.doy).i("year2", t.year2)
         .i("quality", std::clamp(static_cast<int>(std::lround(t.quality * 100.0)), 0, 100))
         .f("offset_ms", offsetMs, 1)
         // The total correction already folded into offset_ms above: the
         // decoder's edge bias plus --extra-delay-ms. A caller that recomputes
         // the offset from last_edge_sample against a better host clock -- as
         // UberSDR's audio extension does -- is replacing the raw difference
         // only, and must add this back or it throws the corrections away with
         // the anchor. Emitted rather than left for the caller to hardcode, so
         // there is one place the figure lives.
         .f("delay_applied_ms", edgeBias + m_o.extraDelayMs, 3)
         .i("last_edge_sample", t.lastEdgeSample)
         // Unrounded, where the decoder resolves it (see edge_sample_exact). A
         // caller re-timing the offset against better timestamps should use
         // this when it is not null.
         .f("last_edge_sample_exact", t.lastEdgeSampleExact, 3)
         .i("frame_start_sample", m_frameStartSample)
         .i("host_anchor_ms", m_anchorMs)
         .str("station", stationName(t.station));
        j.emit();
    }

    void onDiag(const clockdec::ClockDecoderDiagnostics& g,
                clockdec::ClockLockState s, clockdec::ClockStation st,
                std::int64_t samples) {
        Json j;
        j.str("type", "diag")
         .str("state", stateName(s)).str("station", stationName(st))
         .f("tone_snr_db", g.toneSnrDb, 2)
         .f("pwm_contrast", g.pwmContrast, 3)
         .b("tone_detected", g.toneDetected)
         .b("phase_locked", g.phaseLocked)
         .f("delay_est_ms", g.delayEstMs, 2)
         // What the WWV/WWVH station tag is decided from: the folded tick
         // excess of the 2000 Hz band over the 2200 Hz band. Above +1.8 dB
         // leans WWV and below -1.8 dB leans WWVH; a lone WWV reads about
         // +5 dB. Null for WWVB and before the fold locks.
         .f("tick_band_ratio_db", g.tickBandRatioDb, 2)
         .b("anchored", g.anchored)
         .i("bad_frame_streak", g.badFrameStreak)
         .i("frames_in_window", g.framesInWindow)
         .i("window_size", g.windowSize)
         .f("vote_quality", g.voteQuality, 3)
         .str("refusal", refusalName(g.refusalReason))
         .i("samples_consumed", samples);
        if (m_o.decoder == Decoder::Dcf77) {
            // The two demodulators. PM is a 793 ms spread-spectrum correlation
            // that times the second to tens of microseconds and holds through
            // noise that buries the AM; AM is how the minute is marked. Which
            // one is timing the edges, and whether they agree, is the part of a
            // DCF77 lock worth seeing.
            static const char* kFrom[] = {"none", "am", "pm", "both", "disagree"};
            j.b("pm_locked", g.pmLocked)
             .f("pm_snr_db", g.pmSnrDb, 2)
             .str("timing_from", g.timingFromPm ? "pm" : "am")
             .f("am_minus_pm_ms", g.amMinusPmMs, 3)
             .f("carrier_offset_hz", g.carrierOffsetHz, 3)
             .str("last_frame_from", kFrom[std::min<int>(g.lastFrameFrom, 4)])
             .i("pm_refused_locks", g.pmRefusedLocks)
             .b("pm_interference", g.pmInterference);
        } else if (m_o.decoder == Decoder::Allouis) {
            // Allouis is timed by correlating each second's whole phase
            // modulation: whether that correlation is tracking, and how clear.
            j.b("timing_locked", g.pmLocked)
             .f("timing_snr_db", g.pmSnrDb, 2)
             .b("timing", g.timingFromPm)
             .f("carrier_offset_hz", g.carrierOffsetHz, 3);
        } else if (m_o.decoder == Decoder::Msf) {
            j.f("carrier_offset_hz", g.carrierOffsetHz, 3);
        }
        j.emit();
    }

private:
    const Options& m_o;
    int m_rate;
    long long m_anchorMs = 0;
    bool m_haveAnchor = false;
    std::int64_t m_frameStartSample = 0;
    bool m_haveFrame = false;
};

void emitError(std::string_view message) {
    Json j;
    j.str("type", "error").str("message", message);
    j.emit();
}

// ---------------------------------------------------------------------------

// The decoders' constructors differ only in the IQ ones taking the carrier offset.
template <typename D> D* makeDecoder(const Options& o) { return new D(o.sampleRate); }
template <> clockdec::Dcf77Decoder* makeDecoder<clockdec::Dcf77Decoder>(const Options& o) {
    return new clockdec::Dcf77Decoder(o.sampleRate, o.carrierOffsetHz);
}
template <> clockdec::MsfDecoder* makeDecoder<clockdec::MsfDecoder>(const Options& o) {
    return new clockdec::MsfDecoder(o.sampleRate, o.carrierOffsetHz);
}
template <> clockdec::AllouisDecoder* makeDecoder<clockdec::AllouisDecoder>(const Options& o) {
    return new clockdec::AllouisDecoder(o.sampleRate, o.carrierOffsetHz);
}

template <typename D>
int run(const Options& o) {
    std::unique_ptr<D> owned(makeDecoder<D>(o));
    D& decoder = *owned;
    Emitter em(o, o.sampleRate);

    decoder.onStateChanged = [&](clockdec::ClockLockState s) { em.onState(s, decoder.station()); };
    decoder.onSecond = [&](const clockdec::ClockSecondInfo& i) { em.onSecond(i, decoder.station()); };
    decoder.onFrame = [&](const clockdec::ClockFrameInfo& f) { em.onFrame(f); };
    decoder.onTime = [&](const clockdec::ClockTimeInfo& t) { em.onTime(t); };

    if (o.plausibilityMinutes > 0)
        decoder.setPlausibility(hostNowFields, o.plausibilityMinutes);

    // A frame is one sample of every channel: one int16 of mono audio, or an
    // I and a Q. Sample indices everywhere (edge_sample and the rest) count
    // frames, so they mean the same instant whichever the input is.
    const std::size_t channels = isIq(o.decoder) ? 2 : 1;
    const std::size_t frameBytes = 2 * channels;

    constexpr std::size_t kChunkFrames = 4096;
    std::vector<unsigned char> raw(kChunkFrames * frameBytes);
    std::vector<float> samples(kChunkFrames * channels);

    bool anchored = false;
    // Bytes of a frame split across two reads, carried to the next one. A pipe
    // makes no promise to deliver whole frames, and dropping the tail would
    // swap I and Q for the rest of the stream.
    std::size_t carry = 0;
    const std::int64_t diagEvery =
        o.diagSeconds > 0 ? static_cast<std::int64_t>(o.diagSeconds) * o.sampleRate : 0;
    std::int64_t nextDiag = diagEvery;

    for (;;) {
        const std::size_t offset = carry;

        const std::size_t got = std::fread(raw.data() + offset, 1, raw.size() - offset, stdin);
        if (got == 0) {
            if (std::ferror(stdin)) {
                emitError("stdin read error");
                return 1;
            }
            break;   // clean EOF — the caller closed the pipe
        }

        if (!anchored) {
            // The wall clock as the first sample arrives. Everything before
            // this point (receiver buffering, pipe latency) is invisible here
            // and shows up as a constant term in offset_ms; see the note at
            // the top of this file.
            em.setAnchor(hostNowMs());
            anchored = true;
        }

        const std::size_t avail = offset + got;
        const std::size_t nFrames = avail / frameBytes;
        const std::size_t nValues = nFrames * channels;

        for (std::size_t n = 0; n < nValues; ++n) {
            const auto lo = static_cast<std::uint16_t>(raw[2 * n]);
            const auto hi = static_cast<std::uint16_t>(raw[2 * n + 1]);
            const auto v = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8)));
            samples[n] = static_cast<float>(v) * (1.0f / 32768.0f);
        }

        carry = avail - nFrames * frameBytes;
        if (carry) std::memmove(raw.data(), raw.data() + nFrames * frameBytes, carry);

        decoder.process(samples.data(), nFrames);

        if (diagEvery > 0 && decoder.samplesConsumed() >= nextDiag) {
            em.onDiag(decoder.diagnostics(), decoder.state(), decoder.station(),
                      decoder.samplesConsumed());
            // Advance past the current point rather than by one step: a large
            // read must not queue up a burst of backdated diagnostics.
            while (nextDiag <= decoder.samplesConsumed()) nextDiag += diagEvery;
        }
    }

    return 0;
}

void usage() {
    std::printf(
        "ubersdr-clock %s — WWV/WWVH/WWVB/DCF77/MSF/Allouis time-code decoder\n"
        "\n"
        "Reads raw int16 little-endian PCM on stdin -- mono for WWV/WWVH/WWVB,\n"
        "interleaved I/Q for DCF77, MSF and Allouis -- and writes newline-delimited\n"
        "JSON events on stdout. Close stdin to stop.\n"
        "\n"
        "Options:\n"
        "  --sample-rate HZ           Input PCM rate (default: 12000)\n"
        "  --station NAME             wwv, wwvh, wwvb, dcf77, msf or allouis (default: wwv)\n"
        "  --carrier-offset-hz HZ     DCF77, MSF, Allouis: where the carrier sits in the\n"
        "                             baseband, i.e. carrier minus the dial (default: 0)\n"
        "  --no-seconds               Suppress the per-second classification events\n"
        "  --envelope                 Include the 1 s alignment arrays in second events\n"
        "  --diag-seconds N           Diagnostics event every N seconds, 0 = off (default: 10)\n"
        "  --plausibility-minutes N   Refuse a lock more than N minutes from the host\n"
        "                             clock, 0 = disarm (default: 1440)\n"
        "  --extra-delay-ms MS        One-way delay this process cannot see, added back\n"
        "                             to offset_ms (default: 0). For a receiver this is\n"
        "                             mostly the ionospheric path from the transmitter,\n"
        "                             which nothing here can compute -- see the README.\n"
        "                             The decoder's own edge bias is already corrected.\n"
        "  --version                  Print the version and exit\n"
        "  --help                     Print this and exit\n"
        "\n"
        "Tuning:\n"
        "  WWV/WWVH  USB at (carrier - 1 kHz), e.g. 9.999 MHz for the 10 MHz outlet.\n"
        "  WWVB      USB at 0.059 MHz.\n"
        "  DCF77     IQ at 0.0775 MHz: the carrier at 0 Hz of the baseband. The\n"
        "            decoder uses its phase as well as its amplitude, which USB\n"
        "            audio does not carry.\n"
        "  MSF       IQ at 0.060 MHz: timed on the carrier's own coherent amplitude.\n"
        "  Allouis   IQ at 0.162 MHz: phase modulation only.\n"
        "  The passband must reach 2.2 kHz: the WWV/WWVH second tick is recovered\n"
        "  from its 2000 Hz (WWV) / 2200 Hz (WWVH) image, and without it the\n"
        "  decoder never gets a second edge to classify against.\n",
        kVersion);
}

// Parses a signed millisecond argument. Separate from parseInt: this one is
// fractional and may be negative -- a caller that has measured its chain can
// legitimately hand back a correction in either direction.
bool parseMs(const char* opt, const char* text, double& out) {
    if (text == nullptr) {
        std::fprintf(stderr, "ubersdr-clock: %s requires a value\n", opt);
        return false;
    }
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    // A whole second of unseen one-way delay is not a path, it is a mistake --
    // and silently accepting one would move the served second.
    if (end == text || *end != '\0' || !std::isfinite(v) || v < -1000.0 || v > 1000.0) {
        std::fprintf(stderr, "ubersdr-clock: %s: not a valid millisecond value "
                             "in -1000..1000: %s\n", opt, text);
        return false;
    }
    out = v;
    return true;
}

// Parses an integer argument, or reports which option was wrong and why.
bool parseInt(const char* opt, const char* text, int& out) {
    if (text == nullptr) {
        std::fprintf(stderr, "ubersdr-clock: %s requires a value\n", opt);
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 0 || v > 100000000L) {
        std::fprintf(stderr, "ubersdr-clock: %s: not a valid number: %s\n", opt, text);
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        if (a == "--help" || a == "-h") { usage(); return 0; }
        if (a == "--version") { std::printf("%s\n", kVersion); return 0; }

        if (a == "--sample-rate") {
            if (!parseInt("--sample-rate", next, o.sampleRate)) return 2;
            ++i;
        } else if (a == "--station") {
            if (next == nullptr) {
                std::fprintf(stderr, "ubersdr-clock: --station requires a value\n");
                return 2;
            }
            const std::string_view s = next;
            if (s == "wwvb") {
                o.decoder = Decoder::Wwvb;
            } else if (s == "dcf77") {
                o.decoder = Decoder::Dcf77;
            } else if (s == "msf") {
                o.decoder = Decoder::Msf;
            } else if (s == "allouis") {
                o.decoder = Decoder::Allouis;
            } else if (s == "wwv" || s == "wwvh") {
                // One decoder covers both: it tags the station itself from
                // which tick band folds to an impulse.
                o.decoder = Decoder::Wwv;
            } else {
                std::fprintf(stderr, "ubersdr-clock: unknown station: %s "
                                     "(expected wwv, wwvh, wwvb, dcf77, msf or allouis)\n", next);
                return 2;
            }
            ++i;
        } else if (a == "--carrier-offset-hz") {
            if (next == nullptr) {
                std::fprintf(stderr, "ubersdr-clock: --carrier-offset-hz requires a value\n");
                return 2;
            }
            char* end = nullptr;
            const double v = std::strtod(next, &end);
            if (end == next || *end != '\0' || !std::isfinite(v)) {
                std::fprintf(stderr, "ubersdr-clock: --carrier-offset-hz: not a number: %s\n", next);
                return 2;
            }
            o.carrierOffsetHz = v;
            ++i;
        } else if (a == "--no-seconds") {
            o.seconds = false;
        } else if (a == "--envelope") {
            o.envelope = true;
            o.seconds = true;   // the arrays live on the second event
        } else if (a == "--diag-seconds") {
            if (!parseInt("--diag-seconds", next, o.diagSeconds)) return 2;
            ++i;
        } else if (a == "--plausibility-minutes") {
            if (!parseInt("--plausibility-minutes", next, o.plausibilityMinutes)) return 2;
            ++i;
        } else if (a == "--extra-delay-ms") {
            if (!parseMs("--extra-delay-ms", next, o.extraDelayMs)) return 2;
            ++i;
        } else {
            std::fprintf(stderr, "ubersdr-clock: unknown option: %s "
                                 "(try --help)\n", argv[i]);
            return 2;
        }
    }

    // The WWV chain needs the 2000/2200 Hz tick images, so Nyquist has to clear
    // 2.2 kHz with room for the tick bandpass skirts; WWVB only needs its
    // ~1 kHz tone, and DCF77's carrier sits near 0 Hz of a complex baseband.
    // All decimate to a fixed series rate (200 Hz for WWV/WWVH, 100 Hz for the
    // other two), so a rate that is not a multiple of it decimates unevenly and
    // drifts.
    // Allouis folds its phase at 1 kHz to find the second, so its rate must
    // divide into that evenly too.
    const char* name = o.decoder == Decoder::Wwv ? "WWV/WWVH"
                     : o.decoder == Decoder::Wwvb ? "WWVB"
                     : o.decoder == Decoder::Msf ? "MSF"
                     : o.decoder == Decoder::Allouis ? "Allouis" : "DCF77";
    const int seriesRate = o.decoder == Decoder::Wwv ? 200 : o.decoder == Decoder::Allouis ? 1000 : 100;
    const int minRate = o.decoder == Decoder::Wwv ? 8000 : 4000;
    if (o.sampleRate < minRate) {
        std::fprintf(stderr, "ubersdr-clock: --sample-rate %d is too low for %s "
                             "(need at least %d Hz)\n", o.sampleRate, name, minRate);
        return 2;
    }
    if (o.sampleRate % seriesRate != 0) {
        std::fprintf(stderr, "ubersdr-clock: --sample-rate %d is not a multiple of %d Hz "
                             "(the %s series rate); decimation would drift\n",
                     o.sampleRate, seriesRate, name);
        return 2;
    }
    // The carrier has to be inside the baseband, with room for the +/-20 Hz
    // search and the AM filter's skirt either side of it.
    if (isIq(o.decoder) && std::fabs(o.carrierOffsetHz) > o.sampleRate / 2.0 - 200.0) {
        std::fprintf(stderr, "ubersdr-clock: --carrier-offset-hz %.1f puts the carrier outside "
                             "a %d Hz baseband\n", o.carrierOffsetHz, o.sampleRate);
        return 2;
    }

    switch (o.decoder) {
        case Decoder::Wwvb:  return run<clockdec::WwvbDecoder>(o);
        case Decoder::Dcf77: return run<clockdec::Dcf77Decoder>(o);
        case Decoder::Msf:   return run<clockdec::MsfDecoder>(o);
        case Decoder::Allouis: return run<clockdec::AllouisDecoder>(o);
        default:             return run<clockdec::WwvDecoder>(o);
    }
}
