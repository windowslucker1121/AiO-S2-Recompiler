// SPDX-License-Identifier: GPL-3.0-or-later
// On-demand shader translation: a persistent `spirv_harness --serve` child process (tools/shaders/harness/Serve.cs has the protocol).
//
// The shader cache hands it a code block the cache has no translation for; the harness writes <hash>.spv and appends the block's row to
// results.tsv in the same cache folder (so the next run has it) and sends the row back. Nothing here blocks the caller: submit() queues
// the request for a writer thread, a reader thread takes the replies and calls `on_row`. Plain C++ and Win32, no runtime dependencies,
// so a test program can build this file alone.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace nxrt::nvn::be {

class ShaderTranslatorClient {
public:
    struct Options {
        std::string harness;     // spirv_harness.exe
        std::string cache_dir;   // the folder with results.tsv and the .spv files
        std::string corpus_dir;  // the setup's corpus (unique_all.tsv, code/, ctrl/, glslc/); empty: the bytes alone
        int jobs = 2;
        std::string priority = "below";   // below | idle | normal
    };
    enum class State : uint8_t { Unknown, Pending, Done, Failed };
    // hash, the results.tsv row (empty when the harness refused the block), ok = a .spv was written. Called on the reader thread
    // before the block's state turns Done/Failed.
    using RowFn = std::function<void(const std::string& hash, const std::string& row, bool ok)>;
    using LogFn = std::function<void(bool warn, const std::string& text)>;

    ShaderTranslatorClient() = default;
    ~ShaderTranslatorClient();
    ShaderTranslatorClient(const ShaderTranslatorClient&) = delete;
    ShaderTranslatorClient& operator=(const ShaderTranslatorClient&) = delete;

    // Starts the harness (below-normal priority, no window, ended with this process through a job object).
    bool start(const Options& opt, RowFn on_row, LogFn log, std::string* error = nullptr);
    // Queues a block. False when it was asked for before or the harness is not running. Never blocks on the pipe.
    bool submit(const std::string& hash, const uint8_t* code, size_t code_size, const uint8_t* ctrl, size_t ctrl_size);
    // Queues a block of the corpus by hash (the harness reads its files): the shader setup's way to translate the whole corpus.
    bool submit_corpus(const std::string& hash);
    State state(const std::string& hash) const;
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }   // +1 per finished block
    bool running() const { return running_.load(std::memory_order_acquire); }
    size_t pending() const;
    uint64_t done() const { return done_.load(std::memory_order_relaxed); }
    uint64_t failed() const { return failed_.load(std::memory_order_relaxed); }
    void stop();   // closes the harness's input (it finishes the block in hand) and waits for the threads

private:
    void writer();
    void reader();
    void finish(const std::string& hash, bool ok);

    RowFn on_row_;
    LogFn log_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> out_;                            // lines to write
    std::unordered_map<std::string, State> states_;
    size_t pending_ = 0;
    bool stopping_ = false;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0}, done_{0}, failed_{0};
    void* process_ = nullptr;   // HANDLEs
    void* job_ = nullptr;
    void* in_w_ = nullptr;
    void* out_r_ = nullptr;
    std::thread writer_, reader_;
};

std::string base64_encode(const uint8_t* data, size_t size);

}  // namespace nxrt::nvn::be
