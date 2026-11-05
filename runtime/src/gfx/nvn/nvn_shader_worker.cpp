// SPDX-License-Identifier: GPL-3.0-or-later
// See nvn_shader_worker.h.
#include "nvn_shader_worker.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace nxrt::nvn::be {

std::string base64_encode(const uint8_t* p, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += k[v >> 18]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63]; out += k[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = uint32_t(p[i]) << 16;
        out += k[v >> 18]; out += k[(v >> 12) & 63]; out += "==";
    } else if (n - i == 2) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8);
        out += k[v >> 18]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63]; out += '=';
    }
    return out;
}

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring quote(const std::string& s) { return L"\"" + widen(s) + L"\""; }

// The current environment plus DOTNET_GCHeapCount (hex): the harness uses server GC, one heap per core unless capped.
std::vector<wchar_t> environment_with_heaps(int heaps) {
    std::vector<wchar_t> env;
    if (wchar_t* cur = GetEnvironmentStringsW()) {
        for (const wchar_t* p = cur; *p; p += wcslen(p) + 1)
            if (_wcsnicmp(p, L"DOTNET_GCHeapCount=", 19) != 0) env.insert(env.end(), p, p + wcslen(p) + 1);
        FreeEnvironmentStringsW(cur);
    }
    wchar_t var[64];
    swprintf(var, 64, L"DOTNET_GCHeapCount=%x", heaps < 1 ? 1 : heaps);
    env.insert(env.end(), var, var + wcslen(var) + 1);
    env.push_back(L'\0');
    return env;
}
}  // namespace

bool ShaderTranslatorClient::start(const Options& opt, RowFn on_row, LogFn log, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (running()) return fail("already running");
    on_row_ = std::move(on_row);
    log_ = std::move(log);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 20)) return fail("CreatePipe failed");
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 16)) {
        CloseHandle(in_r); CloseHandle(in_w);
        return fail("CreatePipe failed");
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);   // our ends stay ours
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring cmd = quote(opt.harness) + L" --serve --cache " + quote(opt.cache_dir) + L" --jobs " + std::to_wstring(opt.jobs < 1 ? 1 : opt.jobs) +
                       L" --priority " + widen(opt.priority);
    if (!opt.corpus_dir.empty()) cmd += L" --corpus " + quote(opt.corpus_dir);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> env = environment_with_heaps(opt.jobs);
    const DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env.data(), nullptr, &si, &pi);
    const DWORD err = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_w); CloseHandle(out_r);
        return fail("could not start " + opt.harness + " (error " + std::to_string(err) + ")");
    }
    // Ends with this process, however it ends.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        if (AssignProcessToJobObject(job, pi.hProcess)) job_ = job;
        else CloseHandle(job);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    in_w_ = in_w;
    out_r_ = out_r;
    stopping_ = false;
    running_.store(true, std::memory_order_release);
    writer_ = std::thread([this] { writer(); });
    reader_ = std::thread([this] { reader(); });
    return true;
}

bool ShaderTranslatorClient::submit(const std::string& hash, const uint8_t* code, size_t code_size, const uint8_t* ctrl, size_t ctrl_size) {
    if (!running()) return false;
    {
        std::lock_guard lk(mu_);
        auto [it, fresh] = states_.emplace(hash, State::Pending);
        if (!fresh) return false;
        ++pending_;
    }
    // Encoded outside the lock (a block is tens of KB).
    std::string line = "T " + hash + " " + base64_encode(code, code_size) + " " + base64_encode(ctrl, ctrl_size) + "\n";
    {
        std::lock_guard lk(mu_);
        out_.push_back(std::move(line));
    }
    cv_.notify_one();
    return true;
}

bool ShaderTranslatorClient::submit_corpus(const std::string& hash) {
    if (!running()) return false;
    {
        std::lock_guard lk(mu_);
        if (!states_.emplace(hash, State::Pending).second) return false;
        ++pending_;
        out_.push_back("C " + hash + "\n");
    }
    cv_.notify_one();
    return true;
}

ShaderTranslatorClient::State ShaderTranslatorClient::state(const std::string& hash) const {
    std::lock_guard lk(mu_);
    auto it = states_.find(hash);
    return it == states_.end() ? State::Unknown : it->second;
}

size_t ShaderTranslatorClient::pending() const {
    std::lock_guard lk(mu_);
    return pending_;
}

void ShaderTranslatorClient::writer() {
    for (;;) {
        std::string line;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [this] { return stopping_ || !out_.empty(); });
            if (out_.empty()) break;   // stopping and nothing left
            line = std::move(out_.front());
            out_.pop_front();
        }
        const char* p = line.data();
        size_t left = line.size();
        while (left) {
            DWORD wrote = 0;
            if (!WriteFile(static_cast<HANDLE>(in_w_), p, static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left), &wrote, nullptr)) {
                if (log_) log_(true, "shader translator: its input closed (the process ended)");
                return;
            }
            p += wrote;
            left -= wrote;
        }
    }
    // Closing the input tells the harness to stop after the block in hand.
    CloseHandle(static_cast<HANDLE>(in_w_));
    in_w_ = nullptr;
}

void ShaderTranslatorClient::finish(const std::string& hash, bool ok) {
    {
        std::lock_guard lk(mu_);
        auto it = states_.find(hash);
        if (it != states_.end() && it->second == State::Pending && pending_) --pending_;
        states_[hash] = ok ? State::Done : State::Failed;
    }
    (ok ? done_ : failed_).fetch_add(1, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void ShaderTranslatorClient::reader() {
    std::string buf;
    char chunk[1 << 14];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(out_r_), chunk, sizeof chunk, &got, nullptr) || got == 0) break;
        buf.append(chunk, got);
        size_t start = 0;
        for (size_t nl; (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string line = buf.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 2 && line[0] == 'R' && line[1] == '\t') {
                std::string row = line.substr(2);
                const std::string hash = row.substr(0, row.find('\t'));
                // status = column 5 (hash group stage stages_using status ...)
                std::string status;
                size_t c = 0;
                int col = 0;
                for (; col < 4; ++col) {
                    c = row.find('\t', c);
                    if (c == std::string::npos) break;
                    ++c;
                }
                if (col == 4) status = row.substr(c, row.find('\t', c) - c);
                const bool ok = status == "ok" || status == "ok-retry";
                if (on_row_) on_row_(hash, row, ok);
                finish(hash, ok);
            } else if (line.size() > 2 && line[0] == 'X' && line[1] == ' ') {
                const std::string rest = line.substr(2);
                const std::string hash = rest.substr(0, rest.find(' '));
                if (log_) log_(true, "shader translator refused " + rest);
                if (on_row_) on_row_(hash, std::string(), false);
                finish(hash, false);
            } else if (line.size() > 2 && line[0] == 'I' && line[1] == ' ') {
                if (log_) log_(false, "shader translator: " + line.substr(2));
            }
        }
        buf.erase(0, start);
    }
    // The harness is gone: what it had not answered will never be translated in this run.
    running_.store(false, std::memory_order_release);
    std::vector<std::string> lost;
    {
        std::lock_guard lk(mu_);
        for (auto& [h, s] : states_)
            if (s == State::Pending) lost.push_back(h);
    }
    for (const std::string& h : lost) finish(h, false);
    bool was_stopping;
    {
        std::lock_guard lk(mu_);
        was_stopping = stopping_;
        stopping_ = true;
    }
    if (!was_stopping && log_) log_(true, "shader translator exited; " + std::to_string(lost.size()) + " blocks left untranslated");
    cv_.notify_all();
}

void ShaderTranslatorClient::stop() {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
        out_.clear();
    }
    cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (process_) WaitForSingleObject(static_cast<HANDLE>(process_), 5000);   // it finishes the block in hand
    if (job_) {
        CloseHandle(static_cast<HANDLE>(job_));   // kills it if it is still there
        job_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    if (out_r_) CloseHandle(static_cast<HANDLE>(out_r_));
    if (in_w_) CloseHandle(static_cast<HANDLE>(in_w_));
    if (process_) CloseHandle(static_cast<HANDLE>(process_));
    out_r_ = in_w_ = process_ = nullptr;
    running_.store(false, std::memory_order_release);
}

ShaderTranslatorClient::~ShaderTranslatorClient() { stop(); }

}  // namespace nxrt::nvn::be
