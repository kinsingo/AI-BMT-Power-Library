/**
 * @file device/soc_apple.h
 * @brief Apple Silicon SoC energy monitoring backend.
 *
 * Reference: zeus/device/soc/apple.py
 *
 * Uses Apple's private IOReport framework to read energy metrics from
 * Apple Silicon SoCs.  Metrics include:
 *   - apple_cpu_total  : efficiency + performance core clusters (ECPU*, PCPU*)
 *   - apple_gpu        : integrated GPU (GPU0, GPU1, …)
 *   - apple_gpu_sram   : GPU SRAM subsystem
 *   - apple_dram       : DRAM controller (DRAM0, …)
 *   - apple_ane        : Apple Neural Engine (ANE0, …)
 *
 * Implementation
 * ──────────────
 *   IOReport functions are loaded at runtime via dlsym from IOKit.framework.
 *   On construction we subscribe to the "Energy Model" channel group and take
 *   a baseline sample.  Each call to snapshot_energy_j() takes a new sample,
 *   computes the delta from the baseline, and converts to Joules using each
 *   channel's unit label (nJ, mJ, etc.) via IOReportChannelGetUnitLabel.
 *
 * Platform requirements
 * ─────────────────────
 *   - macOS on ARM64 (Apple Silicon)
 *   - IOKit.framework linked at build time (CMake handles this)
 *
 * Non-macOS / x86 macOS: constructor throws std::runtime_error.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(__APPLE__) && defined(__arm64__)
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#endif

namespace zeus
{

    /**
     * @brief Apple Silicon SoC energy backend via IOReport private API.
     *
     * Energy is measured cumulatively from the moment the backend is constructed.
     * snapshot_energy_j() returns the map {metric_key → Joules since ctor}.
     *
     * Thread-safety: snapshot_energy_j() allocates fresh CF objects per call and
     * reads only the immutable baseline, so concurrent calls are safe.
     */
    class AppleSoCBackend
    {
    public:
        // ------------------------------------------------------------------ //
        //  Static helpers                                                      //
        // ------------------------------------------------------------------ //

        /** Compiled for macOS ARM64? */
        static bool is_compiled()
        {
#if defined(__APPLE__) && defined(__arm64__)
            return true;
#else
            return false;
#endif
        }

        /** Runtime check: can we actually load IOReport functions? */
        static bool is_available()
        {
#if defined(__APPLE__) && defined(__arm64__)
            void *lib = open_ioreport_lib();
            if (!lib)
                return false;
            bool ok = (dlsym(lib, "IOReportCopyChannelsInGroup") != nullptr);
            dlclose(lib);
            return ok;
#else
            return false;
#endif
        }

        // ------------------------------------------------------------------ //
        //  Construction / destruction                                          //
        // ------------------------------------------------------------------ //

        AppleSoCBackend()
#if defined(__APPLE__) && defined(__arm64__)
            : lib_(nullptr), subscription_(nullptr), subbed_channels_(nullptr), baseline_sample_(nullptr)
#endif
        {
#if defined(__APPLE__) && defined(__arm64__)
            // 1. Load IOReport library ----------------------------------------
            lib_ = open_ioreport_lib();
            if (!lib_)
            {
                throw std::runtime_error(
                    "Apple SoC: cannot load IOReport from IOKit.framework. "
                    "Ensure you are running macOS on Apple Silicon.");
            }

            // 2. Resolve all required function pointers -----------------------
            if (!load_functions())
            {
                dlclose(lib_);
                lib_ = nullptr;
                throw std::runtime_error(
                    "Apple SoC: required IOReport functions not found in IOKit. "
                    "This macOS version may not expose IOReport energy APIs.");
            }

            // 3. Discover "Energy Model" channels ----------------------------
            CFDictionaryRef channels = fn_copy_channels_(
                CFSTR("Energy Model"), nullptr, 0, 0, 0);
            if (!channels)
            {
                cleanup();
                throw std::runtime_error(
                    "Apple SoC: IOReportCopyChannelsInGroup('Energy Model') "
                    "returned null.  No energy channels on this device.");
            }

            // 4. Create IOReport subscription --------------------------------
            subscription_ = fn_create_subscription_(
                nullptr,
                reinterpret_cast<CFMutableDictionaryRef>(
                    const_cast<void *>(static_cast<const void *>(channels))),
                &subbed_channels_, 0, nullptr);
            CFRelease(channels);

            if (!subscription_ || !subbed_channels_)
            {
                cleanup();
                throw std::runtime_error(
                    "Apple SoC: IOReportCreateSubscription failed.");
            }

            // 5. Take baseline (time-zero) sample ----------------------------
            baseline_sample_ = fn_create_samples_(
                subscription_, subbed_channels_, nullptr);
            if (!baseline_sample_)
            {
                cleanup();
                throw std::runtime_error(
                    "Apple SoC: failed to take baseline energy sample.");
            }

            // 6. Discover metric keys from baseline --------------------------
            discover_metrics();
            if (available_metrics_.empty())
            {
                cleanup();
                throw std::runtime_error(
                    "Apple SoC: no recognisable energy channels found.  "
                    "This Apple Silicon model may not be fully supported.");
            }
#else
            throw std::runtime_error(
                "Apple Silicon SoC monitoring requires macOS on ARM64. "
                "Current platform is not supported.");
#endif
        }

        ~AppleSoCBackend()
        {
#if defined(__APPLE__) && defined(__arm64__)
            cleanup();
#endif
        }

        AppleSoCBackend(const AppleSoCBackend &) = delete;
        AppleSoCBackend &operator=(const AppleSoCBackend &) = delete;

        // ------------------------------------------------------------------ //
        //  Public query interface                                              //
        // ------------------------------------------------------------------ //

        /** Metric keys that this device actually exposes. */
        std::set<std::string> available_metrics() const
        {
#if defined(__APPLE__) && defined(__arm64__)
            return available_metrics_;
#else
            return {};
#endif
        }

        /**
         * Cumulative energy snapshot (Joules) since construction.
         *
         * Takes a fresh IOReport sample, computes the delta to the baseline
         * taken in the constructor, classifies channels, and converts nJ→J.
         */
        std::map<std::string, double> snapshot_energy_j() const
        {
#if defined(__APPLE__) && defined(__arm64__)
            // Current sample
            CFDictionaryRef current = fn_create_samples_(
                subscription_, subbed_channels_, nullptr);
            if (!current)
            {
                throw std::runtime_error(
                    "Apple SoC: IOReportCreateSamples failed.");
            }

            // Delta from baseline (cumulative since ctor)
            CFDictionaryRef delta = fn_create_samples_delta_(
                baseline_sample_, current, nullptr);
            CFRelease(current);
            if (!delta)
            {
                throw std::runtime_error(
                    "Apple SoC: IOReportCreateSamplesDelta failed.");
            }

            auto result = parse_energy_sample(delta);
            CFRelease(delta);
            return result;
#else
            throw std::runtime_error(
                "Apple SoC not available on this platform.");
#endif
        }

        /**
         * Compute SoC energy delta (Joules) from a start snapshot to now.
         */
        std::map<std::string, double> compute_energy_delta_j(
            const std::map<std::string, double> &start_snap) const
        {
            auto end_snap = snapshot_energy_j();
            std::map<std::string, double> result;
            for (const auto &kv : end_snap)
            {
                auto it = start_snap.find(kv.first);
                double s = (it != start_snap.end()) ? it->second : 0.0;
                result[kv.first] = kv.second - s;
            }
            return result;
        }

        /**
         * Get instantaneous power (Watts) for a specific metric key.
         *
         * IOReport provides energy samples, not instant power.
         * This method takes two samples ~50ms apart and computes:
         *   power = delta_energy / delta_time
         *
         * @param metric  Metric key (e.g., "apple_cpu_total", "apple_gpu")
         */
        double get_instant_power_w(const std::string &metric) const
        {
#if defined(__APPLE__) && defined(__arm64__)
            auto snap1 = snapshot_energy_j();
            auto t1 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto snap2 = snapshot_energy_j();
            auto t2 = std::chrono::steady_clock::now();

            auto it1 = snap1.find(metric);
            auto it2 = snap2.find(metric);
            if (it1 == snap1.end() || it2 == snap2.end())
            {
                throw std::runtime_error(
                    "Apple SoC: unknown metric '" + metric + "'");
            }

            double dt = std::chrono::duration<double>(t2 - t1).count();
            if (dt <= 0.0)
                dt = 0.001; // guard against zero
            return (it2->second - it1->second) / dt;
#else
            (void)metric;
            throw std::runtime_error(
                "Apple SoC not available on this platform.");
#endif
        }

        // ================================================================== //
    private:
#if defined(__APPLE__) && defined(__arm64__)

        // ---------- IOReport function-pointer typedefs -------------------- //
        using CopyChannelsFn = CFDictionaryRef (*)(CFStringRef, CFStringRef,
                                                   uint64_t, uint64_t, uint64_t);
        using CreateSubscriptionFn = void *(*)(void *, CFMutableDictionaryRef,
                                               CFMutableDictionaryRef *,
                                               uint64_t, CFTypeRef);
        using CreateSamplesFn = CFDictionaryRef (*)(void *,
                                                    CFMutableDictionaryRef,
                                                    CFTypeRef);
        using CreateSamplesDeltaFn = CFDictionaryRef (*)(CFDictionaryRef,
                                                         CFDictionaryRef,
                                                         CFTypeRef);
        using GetChannelNameFn = CFStringRef (*)(CFDictionaryRef);
        using GetGroupFn = CFStringRef (*)(CFDictionaryRef);
        using GetSubGroupFn = CFStringRef (*)(CFDictionaryRef);
        using GetIntValueFn = int64_t (*)(CFDictionaryRef, int32_t);
        using GetUnitLabelFn = CFStringRef (*)(CFDictionaryRef);

        // ---------- Function pointers (filled by load_functions) ---------- //
        CopyChannelsFn fn_copy_channels_ = nullptr;
        CreateSubscriptionFn fn_create_subscription_ = nullptr;
        CreateSamplesFn fn_create_samples_ = nullptr;
        CreateSamplesDeltaFn fn_create_samples_delta_ = nullptr;
        GetChannelNameFn fn_get_channel_name_ = nullptr;
        GetGroupFn fn_get_group_ = nullptr;
        GetSubGroupFn fn_get_subgroup_ = nullptr;
        GetIntValueFn fn_get_int_value_ = nullptr;
        GetUnitLabelFn fn_get_unit_label_ = nullptr;

        // ---------- IOReport state --------------------------------------- //
        void *lib_ = nullptr;
        void *subscription_ = nullptr;
        CFMutableDictionaryRef subbed_channels_ = nullptr;
        CFDictionaryRef baseline_sample_ = nullptr;
        std::set<std::string> available_metrics_;

        // ================================================================= //
        //  Internal helpers                                                    //
        // ================================================================= //

        /** Try known paths for the library that exports IOReport symbols. */
        static void *open_ioreport_lib()
        {
            // IOReport symbols live inside IOKit.framework on modern macOS
            void *lib = dlopen(
                "/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
            if (lib && dlsym(lib, "IOReportCopyChannelsInGroup"))
                return lib;
            if (lib)
                dlclose(lib);

            // Fallback: standalone dylib (older macOS or custom installs)
            lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_NOW);
            if (lib && dlsym(lib, "IOReportCopyChannelsInGroup"))
                return lib;
            if (lib)
                dlclose(lib);

            // Fallback: private framework
            lib = dlopen(
                "/System/Library/PrivateFrameworks/IOReport.framework/IOReport",
                RTLD_NOW);
            if (lib && dlsym(lib, "IOReportCopyChannelsInGroup"))
                return lib;
            if (lib)
                dlclose(lib);

            return nullptr;
        }

        /** Resolve all IOReport function pointers via dlsym. */
        bool load_functions()
        {
            fn_copy_channels_ = reinterpret_cast<CopyChannelsFn>(
                dlsym(lib_, "IOReportCopyChannelsInGroup"));
            fn_create_subscription_ = reinterpret_cast<CreateSubscriptionFn>(
                dlsym(lib_, "IOReportCreateSubscription"));
            fn_create_samples_ = reinterpret_cast<CreateSamplesFn>(
                dlsym(lib_, "IOReportCreateSamples"));
            fn_create_samples_delta_ = reinterpret_cast<CreateSamplesDeltaFn>(
                dlsym(lib_, "IOReportCreateSamplesDelta"));
            fn_get_channel_name_ = reinterpret_cast<GetChannelNameFn>(
                dlsym(lib_, "IOReportChannelGetChannelName"));
            fn_get_group_ = reinterpret_cast<GetGroupFn>(
                dlsym(lib_, "IOReportChannelGetGroup"));
            fn_get_subgroup_ = reinterpret_cast<GetSubGroupFn>(
                dlsym(lib_, "IOReportChannelGetSubGroup"));
            fn_get_int_value_ = reinterpret_cast<GetIntValueFn>(
                dlsym(lib_, "IOReportSimpleGetIntegerValue"));
            fn_get_unit_label_ = reinterpret_cast<GetUnitLabelFn>(
                dlsym(lib_, "IOReportChannelGetUnitLabel"));

            // fn_get_group_ / fn_get_subgroup_ / fn_get_unit_label_ are optional
            return fn_copy_channels_ && fn_create_subscription_ && fn_create_samples_ && fn_create_samples_delta_ && fn_get_channel_name_ && fn_get_int_value_;
        }

        /** Release all CF / dylib resources. */
        void cleanup()
        {
            if (baseline_sample_)
            {
                CFRelease(baseline_sample_);
                baseline_sample_ = nullptr;
            }
            if (subbed_channels_)
            {
                CFRelease(subbed_channels_);
                subbed_channels_ = nullptr;
            }
            // IOReport subscriptions are opaque pointers — avoid CFRelease
            // to prevent crashes on non-CF types.  Leak is negligible (once
            // per program lifetime).
            subscription_ = nullptr;
            if (lib_)
            {
                dlclose(lib_);
                lib_ = nullptr;
            }
        }

        // ---------- CoreFoundation helpers ------------------------------- //

        /** CFStringRef → std::string  (UTF-8). */
        static std::string cf_to_string(CFStringRef cfStr)
        {
            if (!cfStr)
                return "";
            // Fast path: pointer directly available
            const char *cStr = CFStringGetCStringPtr(cfStr, kCFStringEncodingUTF8);
            if (cStr)
                return std::string(cStr);
            // Slow path: copy into buffer
            CFIndex len = CFStringGetLength(cfStr);
            CFIndex maxSz =
                CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
            std::string buf(static_cast<size_t>(maxSz), '\0');
            if (CFStringGetCString(cfStr, &buf[0], maxSz, kCFStringEncodingUTF8))
            {
                buf.resize(std::strlen(buf.c_str()));
                return buf;
            }
            return "";
        }

        static std::string to_lower(const std::string &s)
        {
            std::string r = s;
            std::transform(r.begin(), r.end(), r.begin(), ::tolower);
            return r;
        }

        // ---------- Unit conversion -------------------------------------- //

        /**
         * Convert a raw integer energy value to Joules based on its unit label.
         *
         * IOReport channels report energy in different units depending on the
         * channel type.  For example, "GPU Energy" typically uses nJ while
         * "CPU Energy", "ECPU*", "DRAM*" etc. use mJ.
         *
         * Unit labels are SI-prefix + J (case-sensitive):
         *   nJ=nano, µJ/uJ=micro, mJ=milli, cJ=centi, dJ=deci, J,
         *   daJ=deca, hJ=hecto, kJ=kilo, MJ=mega, GJ=giga, TJ=tera, PJ=peta
         */
        static double convert_to_joules(int64_t raw_value,
                                        const std::string &unit)
        {
            double val = static_cast<double>(raw_value);
            if (unit == "nJ")
                return val * 1.0e-9;
            if (unit == "\xc2\xb5J" || unit == "uJ")
                return val * 1.0e-6;
            if (unit == "mJ")
                return val * 1.0e-3;
            if (unit == "cJ")
                return val * 1.0e-2;
            if (unit == "dJ")
                return val * 1.0e-1;
            if (unit == "J")
                return val;
            if (unit == "daJ")
                return val * 1.0e1;
            if (unit == "hJ")
                return val * 1.0e2;
            if (unit == "kJ")
                return val * 1.0e3;
            if (unit == "MJ")
                return val * 1.0e6;
            if (unit == "GJ")
                return val * 1.0e9;
            if (unit == "TJ")
                return val * 1.0e12;
            if (unit == "PJ")
                return val * 1.0e15;
            // Fallback: assume nJ (safest default for Apple Silicon)
            return val * 1.0e-9;
        }

        // ---------- Channel classification ------------------------------- //

        /**
         * Check if a channel name is a hardware-provided summary channel.
         * Summary channels (e.g. "CPU Energy", "GPU Energy") already contain
         * the aggregate of individual core channels, so we must not add both.
         */
        static bool is_summary_channel(const std::string &name)
        {
            const std::string low = to_lower(name);
            return (low == "cpu energy" || low == "gpu energy");
        }

        /**
         * Map an IOReport channel name to one of our metric keys.
         *
         * Known "Energy Model" channel names on Apple Silicon (varies by chip):
         *   Summary:  CPU Energy, GPU Energy
         *   M1 Max:   EACC_CPU0, PACC0_CPU0, EACC_CPM, PACC0_CPM, GPU SRAM0, …
         *   M3 Pro:   ECPU0..5, PCPU0..5, ECPM, PCPM, GPU SRAM, DRAM, ANE, …
         *   M4:       ECPU0..5, PCPU0..3, ECPM, PCPM, GPU SRAM, DRAM, ANE, …
         *
         * Returns "" for unrecognised channels (silently skipped).
         */
        static std::string classify_channel(const std::string &name)
        {
            const std::string low = to_lower(name);

            // CPU summary
            if (low == "cpu energy")
            {
                return "apple_cpu_total";
            }
            // CPU individual: efficiency cores, performance cores, core managers
            if (low.find("ecpu") != std::string::npos ||
                low.find("pcpu") != std::string::npos ||
                low.find("eacc") != std::string::npos ||
                low.find("pacc") != std::string::npos ||
                low.find("cpm") != std::string::npos)
            {
                return "apple_cpu_total";
            }
            // GPU SRAM — check before generic "gpu"
            if (low.find("gpu") != std::string::npos &&
                low.find("sram") != std::string::npos)
            {
                return "apple_gpu_sram";
            }
            // GPU summary or individual
            if (low == "gpu energy")
            {
                return "apple_gpu";
            }
            if (low.find("gpu") != std::string::npos)
            {
                return "apple_gpu";
            }
            // DRAM
            if (low.find("dram") != std::string::npos)
            {
                return "apple_dram";
            }
            // ANE (Apple Neural Engine)
            if (low.find("ane") != std::string::npos)
            {
                return "apple_ane";
            }
            return ""; // unknown — skip
        }

        // ---------- Sample parsing --------------------------------------- //

        /**
         * Walk every channel in an IOReport sample (or delta) dictionary,
         * classify it, and accumulate its energy value into the result map.
         *
         * The sample dict contains key "IOReportChannels" → CFArray of
         * per-channel CFDictionaryRef entries.  Energy units vary per channel
         * (e.g. GPU Energy → nJ, CPU/DRAM/ANE → mJ).  We read each channel's
         * unit via IOReportChannelGetUnitLabel and convert to Joules.
         *
         * To avoid double-counting, if a summary channel ("CPU Energy" or
         * "GPU Energy") exists, we use it instead of summing individual core
         * channels for that metric.
         */
        std::map<std::string, double> parse_energy_sample(
            CFDictionaryRef sample) const
        {
            std::map<std::string, double> result;
            if (!sample)
                return result;

            CFArrayRef arr = static_cast<CFArrayRef>(
                CFDictionaryGetValue(sample, CFSTR("IOReportChannels")));
            if (!arr || CFGetTypeID(arr) != CFArrayGetTypeID())
                return result;

            // Two-pass approach to avoid double-counting:
            //   summary_values  : from "CPU Energy", "GPU Energy" (hw aggregate)
            //   individual_values: accumulated from individual core channels
            std::map<std::string, double> summary_values;
            std::map<std::string, double> individual_values;

            const CFIndex n = CFArrayGetCount(arr);
            for (CFIndex i = 0; i < n; ++i)
            {
                CFDictionaryRef ch = static_cast<CFDictionaryRef>(
                    CFArrayGetValueAtIndex(arr, i));
                if (!ch)
                    continue;

                CFStringRef name_cf = fn_get_channel_name_(ch);
                std::string name = cf_to_string(name_cf);
                if (name.empty())
                    continue;

                std::string key = classify_channel(name);
                if (key.empty())
                    continue;

                // Read raw energy value
                int64_t raw = fn_get_int_value_(ch, 0);
                if (raw < 0)
                    raw = 0; // guard against negative deltas

                // Read unit label and convert to Joules
                std::string unit_str = "nJ"; // default assumption
                if (fn_get_unit_label_)
                {
                    CFStringRef unit_cf = fn_get_unit_label_(ch);
                    if (unit_cf)
                    {
                        std::string u = cf_to_string(unit_cf);
                        if (!u.empty())
                            unit_str = u;
                    }
                }
                double joules = convert_to_joules(raw, unit_str);

                // Route to summary or individual bucket
                if (is_summary_channel(name))
                {
                    summary_values[key] = joules; // assign (not accumulate)
                }
                else
                {
                    individual_values[key] += joules; // accumulate cores
                }
            }

            // Merge: prefer hardware summary over individual accumulation
            // For metrics without a summary channel, use individual totals.
            for (const auto &kv : individual_values)
            {
                if (summary_values.find(kv.first) == summary_values.end())
                {
                    result[kv.first] = kv.second;
                }
            }
            for (const auto &kv : summary_values)
            {
                result[kv.first] = kv.second;
            }

            return result;
        }

        /** Populate available_metrics_ by scanning the baseline sample. */
        void discover_metrics()
        {
            if (!baseline_sample_)
                return;

            CFArrayRef arr = static_cast<CFArrayRef>(
                CFDictionaryGetValue(baseline_sample_,
                                     CFSTR("IOReportChannels")));
            if (!arr || CFGetTypeID(arr) != CFArrayGetTypeID())
                return;

            const CFIndex n = CFArrayGetCount(arr);
            for (CFIndex i = 0; i < n; ++i)
            {
                CFDictionaryRef ch = static_cast<CFDictionaryRef>(
                    CFArrayGetValueAtIndex(arr, i));
                if (!ch)
                    continue;

                CFStringRef name_cf = fn_get_channel_name_(ch);
                std::string key = classify_channel(cf_to_string(name_cf));
                if (!key.empty())
                    available_metrics_.insert(key);
            }
        }

#endif // __APPLE__ && __arm64__
    };

} // namespace zeus
