// SPDX-License-Identifier: MIT
#include "xiaotai_fake_parts.hpp"
#include "xiaotai/port/os.h"
#include <algorithm>
#include <cstring>

namespace xiaotai::test {
template<class T> static T& self(void* ptr) { return *static_cast<T*>(ptr); }

static bool owner(std::thread::id& first)
{
    const auto here = std::this_thread::get_id();
    if (first == std::thread::id{}) first = here;
    return first == here;
}

void CameraTrace::Add(CameraOp op)
{
    std::lock_guard lock(mutex);
    if (count < calls.size()) {
        calls[count] = {op, std::this_thread::get_id(), count + 1};
        ++count;
    } else ++dropped;
}

AudioInput::AudioInput(xiaotai_audio_desc_t desc)
{
    part.desc = desc;
    part.self = this;
    part.open = [](void* p) {
        auto& a = self<AudioInput>(p);
        std::lock_guard lock(a.mutex);
        if (a.open_result != XIAOTAI_OK) return a.open_result;
        a.opened = true;
        a.gain = a.part.info.gain_db;
        return XIAOTAI_OK;
    };
    part.read = [](void* p, void* data, size_t capacity, size_t* size) {
        auto& a = self<AudioInput>(p);
        std::lock_guard lock(a.mutex);
        if (!data || !size || !capacity) return XIAOTAI_ERR_INVALID_ARG;
        *size = 0;
        if (!a.opened || !owner(a.reader)) return XIAOTAI_ERR_INVALID_STATE;
        if (a.read_result != XIAOTAI_OK) {
            if (a.read_result != XIAOTAI_ERR_TIMEOUT) ++a.errors;
            return a.read_result;
        }
        if (a.queued.empty()) return XIAOTAI_ERR_TIMEOUT;
        auto& block = a.queued.front();
        const bool pcm = a.part.desc.encoding == XIAOTAI_CODEC_PCM16;
        const size_t frame = sizeof(int16_t) * a.part.desc.channels;
        if (pcm && (!frame || capacity % frame)) return XIAOTAI_ERR_INVALID_SIZE;
        if (pcm && block.size() < capacity) return XIAOTAI_ERR_TIMEOUT;
        if (!pcm && block.size() > capacity) {
            ++a.lost;
            a.queued.pop_front();
            return XIAOTAI_ERR_INVALID_SIZE;
        }
        *size = pcm ? capacity : block.size();
        std::memcpy(data, block.data(), *size);
        if (*size == block.size()) a.queued.pop_front();
        else block.erase(block.begin(), block.begin() + *size);
        return XIAOTAI_OK;
    };
    part.close = [](void* p) {
        auto& a = self<AudioInput>(p);
        std::lock_guard lock(a.mutex);
        a.opened = false;
        a.reader = {};
        a.queued.clear();
        return XIAOTAI_OK;
    };
    part.set_gain_db = [](void* p, int8_t db) {
        auto& a = self<AudioInput>(p);
        const auto& i = a.part.info;
        if (!a.opened) return XIAOTAI_ERR_INVALID_STATE;
        if (db < i.gain_min_db || db > i.gain_max_db) return XIAOTAI_ERR_INVALID_ARG;
        const int step = i.gain_step_db;
        a.gain = step ? static_cast<int8_t>(i.gain_min_db + (db - i.gain_min_db) / step * step) : db;
        return XIAOTAI_OK;
    };
    part.stats = [](void* p, xiaotai_audio_stats_t* stats) {
        const auto& a = self<AudioInput>(p);
        *stats = {a.overruns.load(), a.lost.load(), a.errors.load()};
    };
}

AudioOutput::AudioOutput(xiaotai_audio_desc_t desc)
{
    part.desc = desc;
    part.self = this;
    part.open = [](void* p) {
        auto& a = self<AudioOutput>(p);
        std::lock_guard lock(a.mutex);
        if (a.open_result != XIAOTAI_OK) return a.open_result;
        a.opened = true;
        a.enabled = false;
        return XIAOTAI_OK;
    };
    part.write = [](void* p, void* data, size_t size) {
        auto& a = self<AudioOutput>(p);
        std::lock_guard lock(a.mutex);
        if (!a.opened || !owner(a.writer)) return XIAOTAI_ERR_INVALID_STATE;
        if (!data || !size) return XIAOTAI_ERR_INVALID_ARG;
        const size_t frame = sizeof(int16_t) * a.part.desc.channels;
        const size_t limit = a.part.info.max_write_frames;
        if (!frame || size % frame || (limit && size / frame > limit)) return XIAOTAI_ERR_INVALID_SIZE;
        if (a.write_result != XIAOTAI_OK) {
            ++a.errors;
            return a.write_result;
        }
        a.bytes_written += size;
        return XIAOTAI_OK;
    };
    part.set_volume = [](void* p, uint8_t volume) {
        auto& a = self<AudioOutput>(p);
        if (!a.opened) return XIAOTAI_ERR_INVALID_STATE;
        if (volume > 100) return XIAOTAI_ERR_INVALID_ARG;
        a.volume = volume;
        return XIAOTAI_OK;
    };
    part.set_muted = [](void* p, bool mute) {
        auto& a = self<AudioOutput>(p);
        if (!a.opened) return XIAOTAI_ERR_INVALID_STATE;
        a.muted = mute;
        return XIAOTAI_OK;
    };
    part.set_enabled = [](void* p, bool enabled) {
        auto& a = self<AudioOutput>(p);
        if (!a.opened) return XIAOTAI_ERR_INVALID_STATE;
        a.enabled = enabled;
        return XIAOTAI_OK;
    };
    part.flush_frames = [](void* p) { return self<AudioOutput>(p).flush; };
    part.close = [](void* p) {
        auto& a = self<AudioOutput>(p);
        std::lock_guard lock(a.mutex);
        a.enabled = a.opened = false;
        a.writer = {};
        return XIAOTAI_OK;
    };
    part.stats = [](void* p, xiaotai_audio_stats_t* stats) {
        *stats = {0, 0, self<AudioOutput>(p).errors.load()};
    };
}

Display::Display(xiaotai_display_desc_t desc)
{
    part.desc = desc;
    part.self = this;
    const unsigned w = desc.rotation % 2 ? desc.height : desc.width;
    const unsigned h = desc.rotation % 2 ? desc.width : desc.height;
    const unsigned bpp = desc.pixel == XIAOTAI_PIXEL_RGB565 ? 2 : desc.pixel == XIAOTAI_PIXEL_RGB888 ? 3 : 0;
    const size_t bytes = bpp ? static_cast<size_t>(w) * h * bpp :
        desc.pixel == XIAOTAI_PIXEL_MONO1_VLSB ? w * ((h + 7) / 8) : ((w + 7) / 8) * h;
    visible.resize(bytes);
    if (desc.render == XIAOTAI_RENDER_SCANOUT)
        for (size_t i = 0; i < std::min<size_t>(desc.buffers, scanout.size()); ++i) scanout[i].resize(bytes);
    part.open = [](void* p, xiaotai_display_handles_t* handles) {
        auto& d = self<Display>(p);
        if (!d.opened && d.open_result != XIAOTAI_OK) return d.open_result;
        d.opened = true;
        if (handles) {
            *handles = {};
            for (size_t i = 0; i < d.scanout.size(); ++i)
                if (!d.scanout[i].empty()) handles->framebuffers[i] = d.scanout[i].data();
        }
        return XIAOTAI_OK;
    };
    part.set_backlight = [](void* p, uint8_t percent) {
        auto& d = self<Display>(p);
        if (!d.opened) return XIAOTAI_ERR_INVALID_STATE;
        if (percent > 100) return XIAOTAI_ERR_INVALID_ARG;
        d.backlight = (percent && !(d.part.desc.flags & XIAOTAI_DISPLAY_DIMMABLE)) ? 100 : percent;
        return XIAOTAI_OK;
    };
    part.present = [](void* p, const xiaotai_rect_t* a, const void* data) {
        auto& d = self<Display>(p);
        if (!d.opened) return XIAOTAI_ERR_INVALID_STATE;
        if (!a || !data || !a->width || !a->height) return XIAOTAI_ERR_INVALID_ARG;
        const auto& cfg = d.part.desc;
        const unsigned w = cfg.rotation % 2 ? cfg.height : cfg.width;
        const unsigned h = cfg.rotation % 2 ? cfg.width : cfg.height;
        if (a->x + a->width > w || a->y + a->height > h) return XIAOTAI_ERR_INVALID_ARG;
        const bool whole = !a->x && !a->y && a->width == w && a->height == h;
        if ((!whole && (cfg.render == XIAOTAI_RENDER_SCANOUT || cfg.pixel == XIAOTAI_PIXEL_MONO1_VLSB)) ||
            ((cfg.flags & XIAOTAI_DISPLAY_EVEN_AREAS) && ((a->x | a->width) & 1)))
            return XIAOTAI_ERR_INVALID_ARG;
        if (d.present_result != XIAOTAI_OK) return d.present_result;
        d.visible_buffer = -1;
        if (whole && cfg.render == XIAOTAI_RENDER_SCANOUT) {
            for (size_t i = 0; i < d.scanout.size(); ++i)
                if (data == d.scanout[i].data()) d.visible_buffer = static_cast<int>(i);
        }
        const auto* src = static_cast<const uint8_t*>(data);
        const size_t bpp = cfg.pixel == XIAOTAI_PIXEL_RGB565 ? 2 : cfg.pixel == XIAOTAI_PIXEL_RGB888 ? 3 : 0;
        if (d.visible_buffer < 0) {
            if (whole) std::memmove(d.visible.data(), src, d.visible.size());
            else if (bpp) {
                for (size_t y = 0; y < a->height; ++y)
                    std::memcpy(d.visible.data() + ((a->y + y) * w + a->x) * bpp, src + y * a->width * bpp,
                                a->width * bpp);
            } else {
                for (size_t y = 0; y < a->height; ++y) {
                    for (size_t x = 0; x < a->width; ++x) {
                        const uint8_t from = src[y * ((a->width + 7) / 8) + x / 8] & (0x80U >> (x % 8));
                        const unsigned dst_x = a->x + x;
                        auto& dst = d.visible[(a->y + y) * ((w + 7) / 8) + dst_x / 8];
                        const uint8_t bit = 0x80U >> (dst_x % 8);
                        dst = from ? dst | bit : dst & ~bit;
                    }
                }
            }
        }
        ++d.presents;
        d.last_area = *a;
        return XIAOTAI_OK;
    };
    if (desc.touch) part.read_touch = [](void* p, xiaotai_touch_point_t* points, size_t capacity, size_t* count) {
        auto& d = self<Display>(p);
        if (!d.opened) return XIAOTAI_ERR_INVALID_STATE;
        if (!count || (capacity && !points)) return XIAOTAI_ERR_INVALID_ARG;
        *count = d.touch && capacity ? 1 : 0;
        if (*count) points[0] = *d.touch;
        return XIAOTAI_OK;
    };
}

const uint8_t* Display::showing() const
{
    return visible_buffer >= 0 ? scanout[visible_buffer].data() : visible.data();
}

Input::Input(xiaotai_input_desc_t desc)
{
    part.desc = desc;
    part.self = this;
    part.open = [](void* p) {
        auto& in = self<Input>(p);
        if (in.open_result != XIAOTAI_OK) return in.open_result;
        in.opened = in.first = true;
        in.held = in.part.desc.kind == XIAOTAI_INPUT_BUTTON && in.initially_pressed;
        in.level.reset();
        in.has_data = false;
        return XIAOTAI_OK;
    };
    part.poll = [](void* p) -> uint32_t {
        auto& in = self<Input>(p);
        if (!in.opened || !owner(in.reader)) return 0;
        in.has_data = false;
        if (in.first) {
            in.first = false;
            if (in.part.desc.kind == XIAOTAI_INPUT_CONTACT)
                return in.initially_pressed ? XIAOTAI_INPUT_PRESSED : XIAOTAI_INPUT_RELEASED;
            if (in.part.desc.events & XIAOTAI_INPUT_LEVEL) {
                in.level = std::clamp(in.initial_level, in.part.desc.level_min, in.part.desc.level_max);
                return XIAOTAI_INPUT_LEVEL;
            }
        }
        if (in.queued.empty()) return 0;
        auto sample = std::move(in.queued.front());
        in.queued.pop_front();
        if (in.held) {
            if (sample.events & XIAOTAI_INPUT_RELEASED) in.held = false;
            return 0;
        }
        uint32_t bits = sample.events & in.part.desc.events;
        if (bits & XIAOTAI_INPUT_DATA) {
            in.data = std::move(sample.data);
            if (in.data.size() >= XIAOTAI_INPUT_TEXT_MAX) in.data.resize(XIAOTAI_INPUT_TEXT_MAX - 1);
            in.has_data = true;
        }
        if (bits & XIAOTAI_INPUT_LEVEL)
            in.level = std::clamp(sample.level, in.part.desc.level_min, in.part.desc.level_max);
        return bits;
    };
    if (desc.events & XIAOTAI_INPUT_DATA) part.read = [](void* p, char* text, size_t capacity) {
        auto& in = self<Input>(p);
        if (!in.opened || !owner(in.reader)) return XIAOTAI_ERR_INVALID_STATE;
        if (!text || !capacity) return XIAOTAI_ERR_INVALID_ARG;
        text[0] = '\0';
        if (!in.has_data) return XIAOTAI_ERR_NOT_FOUND;
        if (in.data.size() >= capacity) return XIAOTAI_ERR_INVALID_SIZE;
        std::memcpy(text, in.data.c_str(), in.data.size() + 1);
        in.has_data = false;
        return XIAOTAI_OK;
    };
    if (desc.events & XIAOTAI_INPUT_LEVEL) part.read_level = [](void* p, int32_t* level) {
        auto& in = self<Input>(p);
        if (!in.opened || !owner(in.reader)) return XIAOTAI_ERR_INVALID_STATE;
        if (!level) return XIAOTAI_ERR_INVALID_ARG;
        if (!in.level) return XIAOTAI_ERR_NOT_FOUND;
        *level = *in.level;
        return XIAOTAI_OK;
    };
}

Output::Output(xiaotai_output_desc_t desc) : pixels(desc.pixels * 3U)
{
    part.desc = desc;
    part.self = this;
    part.set = [](void* p, uint8_t r, uint8_t g, uint8_t b) {
        auto& out = self<Output>(p);
        if (out.result != XIAOTAI_OK) return out.result;
        const bool rgb = out.part.desc.kind == XIAOTAI_OUTPUT_RGB_LIGHT;
        if (out.part.desc.kind == XIAOTAI_OUTPUT_RELAY) r = r ? 255 : 0;
        for (size_t i = 0; i < out.pixels.size(); i += 3) {
            out.pixels[i] = r;
            out.pixels[i + 1] = rgb ? g : 0;
            out.pixels[i + 2] = rgb ? b : 0;
        }
        ++out.writes;
        return XIAOTAI_OK;
    };
    if (desc.pixels > 1) part.write = [](void* p, const uint8_t* rgb, size_t count) {
        auto& out = self<Output>(p);
        if (!rgb || count != out.part.desc.pixels) return XIAOTAI_ERR_INVALID_ARG;
        if (out.result != XIAOTAI_OK) return out.result;
        std::copy_n(rgb, out.pixels.size(), out.pixels.begin());
        ++out.writes;
        return XIAOTAI_OK;
    };
}

Camera::Camera(xiaotai_camera_desc_t desc, CameraTrace& calls, bool has_prepare) : trace(calls)
{
    part.desc = desc;
    part.self = this;
    if (has_prepare) part.prepare = [](void* p) {
        auto& c = self<Camera>(p);
        c.trace.Add(CameraOp::Prepare);
        if (!owner(c.worker) || c.prepared || c.power_called) return XIAOTAI_ERR_INVALID_STATE;
        c.prepared = true;
        c.ready = c.prepare_result == XIAOTAI_OK;
        return c.prepare_result;
    };
    part.set_power = [](void* p, bool on) {
        auto& c = self<Camera>(p);
        if (!owner(c.worker) || c.attempted_open || c.loan) return XIAOTAI_ERR_INVALID_STATE;
        c.trace.Add(on ? CameraOp::PowerOn : CameraOp::PowerOff);
        c.power_called = true;
        if (c.power_result != XIAOTAI_OK) return c.power_result;
        if (on && !c.ready) {
            c.trace.Add(CameraOp::Reserve);
            if (c.reserve_result != XIAOTAI_OK) return c.reserve_result;
            c.ready = true;
        }
        c.powered = on;
        return XIAOTAI_OK;
    };
    part.open = [](void* p, const xiaotai_camera_mode_t* mode) {
        auto& c = self<Camera>(p);
        c.trace.Add(CameraOp::Open);
        if (!owner(c.worker) || !c.powered || c.attempted_open) return XIAOTAI_ERR_INVALID_STATE;
        c.attempted_open = true;
        if (!mode || !mode->width || !mode->height) return XIAOTAI_ERR_INVALID_ARG;
        const auto& d = c.part.desc;
        const uint32_t f = mode->format;
        if (!f || (f & (f - 1)) || !(f & d.formats) ||
            mode->width < d.min_width || mode->width > d.max_width ||
            mode->height < d.min_height || mode->height > d.max_height) return XIAOTAI_ERR_NOT_SUPPORTED;
        if (c.open_result != XIAOTAI_OK) return c.open_result;
        c.opened = true;
        return XIAOTAI_OK;
    };
    part.borrow = [](void* p, xiaotai_camera_frame_t* frame) {
        auto& c = self<Camera>(p);
        c.trace.Add(CameraOp::Borrow);
        if (!owner(c.worker) || !c.opened || c.loan) return XIAOTAI_ERR_INVALID_STATE;
        if (!frame) return XIAOTAI_ERR_INVALID_ARG;
        std::lock_guard lock(c.frame_mutex);
        if (!c.newest) return XIAOTAI_ERR_NOT_FOUND;
        *frame = *c.newest;
        c.newest.reset();
        c.loan = frame;
        return XIAOTAI_OK;
    };
    part.release = [](void* p, xiaotai_camera_frame_t* frame) {
        auto& c = self<Camera>(p);
        c.trace.Add(CameraOp::Release);
        if (!owner(c.worker) || !c.loan || c.loan != frame) return XIAOTAI_ERR_INVALID_STATE;
        c.loan = nullptr;
        *frame = {};
        return XIAOTAI_OK;
    };
    part.close = [](void* p) {
        auto& c = self<Camera>(p);
        c.trace.Add(CameraOp::Close);
        if (!owner(c.worker) || c.loan) return XIAOTAI_ERR_INVALID_STATE;
        c.opened = c.attempted_open = false;
        std::lock_guard lock(c.frame_mutex);
        c.newest.reset();
        return XIAOTAI_OK;
    };
}

void Camera::Publish(xiaotai_camera_frame_t frame)
{
    std::lock_guard lock(frame_mutex);
    if (newest) frame.skipped += newest->skipped + 1;
    newest = frame;
}

Network::Network(xiaotai_network_desc_t desc, bool maintenance)
{
    part.desc = desc;
    part.self = this;
    if (desc.kind == XIAOTAI_NETWORK_WIFI || desc.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR)
        part.driver_config = &wifi;
    if (!maintenance) return;
    part.prepare = [](void* p, bool retry, xiaotai_network_progress_fn_t step, void* ctx) {
        auto& n = self<Network>(p);
        ++n.prepares;
        n.last_retry = retry;
        if (step) {
            for (auto stage : n.prepare_stages) {
                if (n.prepare_result != XIAOTAI_OK && (stage == XIAOTAI_NETWORK_READY ||
                    stage == XIAOTAI_NETWORK_RESTARTING)) continue;
                step(stage, 0, ctx);
            }
        }
        return n.prepare_result;
    };
    part.coprocessor_info = [](void* p, bool live, xiaotai_coprocessor_info_t* info) {
        auto& n = self<Network>(p);
        if (!info) return XIAOTAI_ERR_INVALID_ARG;
        n.live_queries += live;
        if (!n.identity.version[0]) return XIAOTAI_ERR_NOT_FOUND;
        *info = n.identity;
        return XIAOTAI_OK;
    };
    part.coprocessor_update = [](void* p, const uint8_t* image, size_t size,
                                xiaotai_network_progress_fn_t step, void* ctx) {
        auto& n = self<Network>(p);
        if (!image || !size) return XIAOTAI_ERR_INVALID_ARG;
        ++n.updates;
        n.last_update_size = size;
        if (step) {
            for (auto stage : n.update_stages) {
                if (n.update_result != XIAOTAI_OK && stage == XIAOTAI_NETWORK_RESTARTING) continue;
                step(stage, stage == XIAOTAI_NETWORK_RESTARTING ? 100 : 0, ctx);
            }
        }
        return n.update_result;
    };
    part.coprocessor_command = [](void* p, const char* command, char* reply, size_t capacity) {
        auto& n = self<Network>(p);
        if (!command || !reply || !capacity) return XIAOTAI_ERR_INVALID_ARG;
        n.last_command = command;
        reply[0] = '\0';
        if (n.command_result != XIAOTAI_OK) return n.command_result;
        const size_t copied = std::min(capacity - 1, n.reply.size());
        std::memcpy(reply, n.reply.data(), copied);
        reply[copied] = '\0';
        return XIAOTAI_OK;
    };
}

Measurement::Measurement(xiaotai_measurement_desc_t desc)
{
    part.desc = desc;
    part.self = this;
    clock_us = xiaotai_time_us;
    part.read = [](void* p, int32_t* value) {
        auto& m = self<Measurement>(p);
        if (!value) return XIAOTAI_ERR_INVALID_ARG;
        std::lock_guard lock(m.mutex);
        const int64_t now = m.clock_us();
        if (!m.sampled_us || now - *m.sampled_us >= static_cast<int64_t>(m.part.desc.sample_ms) * 1000) {
            ++m.samples;
            if (m.sample_result != XIAOTAI_OK) return m.sample_result;
            m.sampled_us = now;
            m.cached = m.sample;
        }
        *value = m.cached;
        return XIAOTAI_OK;
    };
}
}  // namespace xiaotai::test
