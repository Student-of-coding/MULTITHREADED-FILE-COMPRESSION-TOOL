#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <zlib.h>
#include <libdeflate.h>
#include <codecvt>
#include <locale>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "zlib.lib")
#pragma comment(lib, "deflatestatic.lib")

static constexpr uint64_t DEFAULT_CHUNK_SIZE = 4ULL * 1024 * 1024;
static constexpr char MAGIC[8] = { 'M','T','Z','P','0','0','0','1' };
static constexpr int MAX_IN_FLIGHT_FACTOR = 8;

constexpr UINT WM_APP_PROGRESS     = WM_APP + 1;
constexpr UINT WM_APP_MULTI_DONE   = WM_APP + 3;
constexpr UINT WM_APP_SHOW_RESULTS = WM_APP + 4;

enum {
    ID_BTN_SELECT       = 1001,
    ID_EDIT_CHUNKSIZE   = 1002,
    ID_BTN_COMPRESS     = 1003,
    ID_BTN_DECOMPRESS   = 1004,
    ID_PROGRESS         = 1005,
    ID_RESULTS          = 1006,
    ID_FILEPATH         = 1007
};

HINSTANCE      g_hInst         = nullptr;
HWND           g_hMainWnd      = nullptr;
HWND           g_hBtnSelect    = nullptr;
HWND           g_hEditMib      = nullptr;
HWND           g_hBtnCompress  = nullptr;
HWND           g_hBtnDecompress= nullptr;
HWND           g_hProgress     = nullptr;
HWND           g_hResults      = nullptr;
HWND           g_hFilePath     = nullptr;

std::wstring   g_inputPath;
int            g_totalChunks = 0;
volatile int   g_chunksDone  = 0;

struct BenchData {
    int     chunkSizeMiB;
    double  singleTimeMs;
    uint64_t singleOutSize;
    double  multiTimeMs;
    uint64_t multiOutSize;
};
struct DecompData {
    double  multiTimeMs;
    uint64_t multiOutSize;
};

BenchData   g_benchGlobal;
DecompData  g_decompGlobal;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void InitCommonControlsWrapper();
static std::string WideToUtf8(const std::wstring &);
static uint64_t get_file_size(const std::string &);
static void write_exact(std::ofstream &out, const void *buf, size_t n);
static void write_u64_le(std::ofstream &out, uint64_t x);
static uint64_t read_u64_le_from_buffer(const uint8_t *buf);

using ProgressCallback = std::function<void(int)>;

void compress_file_singlethread_cb(
    const std::string &in_path,
    const std::string &out_path,
    uint64_t chunk_size,
    ProgressCallback progress_cb
);

void compress_file_multithread_cb(
    const std::string &in_path,
    const std::string &out_path,
    uint64_t chunk_size,
    ProgressCallback progress_cb
);

void decompress_file_multithread_cb(
    const std::wstring &in_path,
    const std::string &out_path,
    ProgressCallback progress_cb
);

struct CompressJob {
    HWND        hwnd;
    std::string inPath;
    std::string singleOut;
    std::string multiOut;
    uint64_t    chunkSizeBytes;
};
struct DecompressJob {
    HWND        hwnd;
    std::string inPath;
    std::string originalName;
};

static DWORD WINAPI CompressWorker(LPVOID);
static DWORD WINAPI DecompressWorker(LPVOID);

void InitCommonControlsWrapper() {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
}

static std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return {};
    int size_needed = ::WideCharToMultiByte(
        CP_UTF8, 0,
        w.c_str(), (int)w.size(),
        nullptr, 0, nullptr, nullptr
    );
    std::string out(size_needed, '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0,
        w.c_str(), (int)w.size(),
        out.data(), size_needed, nullptr, nullptr
    );
    return out;
}

static uint64_t get_file_size(const std::string &fname) {
    std::ifstream in(fname, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("Cannot open file to get size: " + fname);
    uint64_t size = static_cast<uint64_t>(in.tellg());
    in.close();
    return size;
}

static void write_exact(std::ofstream &out, const void *buf, size_t n) {
    out.write(reinterpret_cast<const char*>(buf), n);
    if (!out) {
        throw std::runtime_error("Failed to write expected number of bytes");
    }
}

static void write_u64_le(std::ofstream &out, uint64_t x) {
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = static_cast<uint8_t>((x >> (8 * i)) & 0xFF);
    }
    write_exact(out, buf, 8);
}

static uint64_t read_u64_le_from_buffer(const uint8_t *buf) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) {
        x |= (static_cast<uint64_t>(buf[i]) << (8 * i));
    }
    return x;
}

static std::vector<uint8_t> compress_chunk(const uint8_t *raw_data, size_t raw_size) {
    uLong bound = compressBound(static_cast<uLong>(raw_size));
    std::vector<uint8_t> out_buf(bound);
    uLongf dest_len = bound;
    int ret = compress2(
        out_buf.data(), &dest_len,
        raw_data, static_cast<uLong>(raw_size),
        Z_BEST_COMPRESSION
    );
    if (ret != Z_OK) {
        throw std::runtime_error("zlib compress2() failed");
    }
    out_buf.resize(dest_len);
    return out_buf;
}

void compress_file_singlethread_cb(
    const std::string &in_path,
    const std::string &out_path,
    uint64_t chunk_size,
    ProgressCallback progress_cb
) {
    std::ifstream infile(in_path, std::ios::binary);
    if (!infile) throw std::runtime_error("Cannot open input file: " + in_path);
    std::string filename_only;
    size_t pos_slash = in_path.find_last_of("\\/");
    if (pos_slash != std::string::npos)
        filename_only = in_path.substr(pos_slash + 1);
    else
        filename_only = in_path;
    std::string tmp_out = out_path + ".tmp";
    std::ofstream outfile(tmp_out, std::ios::binary);
    if (!outfile) throw std::runtime_error("Cannot open output file: " + tmp_out);
    size_t name_len = filename_only.size();
    outfile.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
    outfile.write(filename_only.c_str(), name_len);
    uint64_t original_size = get_file_size(in_path);
    uint64_t num_chunks = (original_size + chunk_size - 1) / chunk_size;
    uint64_t header_size = sizeof(MAGIC) + 8 + 8 + 8 + 8 * num_chunks;
    std::vector<char> header_zero(header_size, 0);
    write_exact(outfile, header_zero.data(), header_size);
    std::vector<uint64_t> comp_sizes(num_chunks, 0);
    for (uint64_t i = 0; i < num_chunks; i++) {
        uint64_t offset = i * chunk_size;
        uint64_t this_size = std::min<uint64_t>(chunk_size, original_size - offset);
        std::vector<uint8_t> raw(this_size);
        infile.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        infile.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(this_size));
        if (infile.gcount() != static_cast<std::streamsize>(this_size)) throw std::runtime_error("Failed to read");
        auto compressed = compress_chunk(raw.data(), raw.size());
        uint64_t csize = compressed.size();
        comp_sizes[i] = csize;
        write_exact(outfile, compressed.data(), compressed.size());
        progress_cb(static_cast<int>(i + 1));
    }
    outfile.seekp(sizeof(name_len) + name_len, std::ios::beg);
    write_exact(outfile, MAGIC, sizeof(MAGIC));
    write_u64_le(outfile, chunk_size);
    write_u64_le(outfile, original_size);
    write_u64_le(outfile, num_chunks);
    for (uint64_t cs : comp_sizes) write_u64_le(outfile, cs);
    outfile.close();
    infile.close();
    std::remove(out_path.c_str());
    std::rename(tmp_out.c_str(), out_path.c_str());
}

void compress_file_multithread_cb(
    const std::string &in_path,
    const std::string &out_path,
    uint64_t chunk_size,
    ProgressCallback progress_cb
) {
    std::ifstream infile(in_path, std::ios::binary);
    if (!infile) throw std::runtime_error("Cannot open input file: " + in_path);
    std::string filename_only;
    size_t pos_slash = in_path.find_last_of("\\/");
    if (pos_slash != std::string::npos)
        filename_only = in_path.substr(pos_slash + 1);
    else
        filename_only = in_path;
    std::string tmp_out = out_path + ".tmp";
    std::ofstream outfile(tmp_out, std::ios::binary);
    if (!outfile) throw std::runtime_error("Cannot open output file: " + tmp_out);
    size_t name_len = filename_only.size();
    outfile.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
    outfile.write(filename_only.c_str(), name_len);
    uint64_t original_size = get_file_size(in_path);
    uint64_t num_chunks = (original_size + chunk_size - 1) / chunk_size;
    uint64_t header_size = sizeof(MAGIC) + 8 + 8 + 8 + 8 * num_chunks;
    std::vector<char> header_zero(header_size, 0);
    write_exact(outfile, header_zero.data(), header_size);
    std::vector<uint64_t> comp_sizes(num_chunks, 0);
    std::mutex write_mutex;
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (num_threads < 1) num_threads = 2;
    class Semaphore {
    public:
        explicit Semaphore(int count = 0) : count_(count) {}
        void acquire() {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this]() { return count_ > 0; });
            --count_;
        }
        void release() {
            std::unique_lock<std::mutex> lk(mtx_);
            ++count_;
            cv_.notify_one();
        }
    private:
        std::mutex                 mtx_;
        std::condition_variable    cv_;
        int                        count_;
    } sem(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_chunks));
    for (uint64_t i = 0; i < num_chunks; i++) {
        uint64_t offset = i * chunk_size;
        uint64_t this_size = std::min<uint64_t>(chunk_size, original_size - offset);
        std::vector<uint8_t> raw(this_size);
        infile.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        infile.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(this_size));
        if (infile.gcount() != static_cast<std::streamsize>(this_size)) throw std::runtime_error("Failed to read");
        sem.acquire();
        threads.emplace_back([
            i, this_size, raw = std::move(raw), &outfile, &comp_sizes,
            &write_mutex, &sem, progress_cb
        ]() mutable {
            try {
                auto compressed = compress_chunk(raw.data(), this_size);
                uint64_t csize = compressed.size();
                {
                    std::lock_guard<std::mutex> lg(write_mutex);
                    outfile.seekp(0, std::ios::end);
                    write_exact(outfile, compressed.data(), compressed.size());
                    comp_sizes[i] = csize;
                }
                progress_cb(static_cast<int>(i + 1));
            } catch (...) {}
            sem.release();
        });
    }
    infile.close();
    for (auto &t : threads) if (t.joinable()) t.join();
    outfile.seekp(sizeof(name_len) + name_len, std::ios::beg);
    write_exact(outfile, MAGIC, sizeof(MAGIC));
    write_u64_le(outfile, chunk_size);
    write_u64_le(outfile, original_size);
    write_u64_le(outfile, num_chunks);
    for (uint64_t cs : comp_sizes) write_u64_le(outfile, cs);
    outfile.close();
    std::remove(out_path.c_str());
    std::rename(tmp_out.c_str(), out_path.c_str());
}

struct Chunk {
    uint64_t index;
    std::vector<uint8_t> compData;
    uint64_t uncompressedSize;
};

class BoundedQueue {
private:
    std::mutex                 mtx;
    std::condition_variable    cv_not_full;
    std::condition_variable    cv_not_empty;
    std::queue<Chunk>          queue_data;
    int                        capacity;
    bool                       finished;
public:
    explicit BoundedQueue(int cap) : capacity(cap), finished(false) {}
    void push(Chunk&& chunk) {
        std::unique_lock<std::mutex> lk(mtx);
        cv_not_full.wait(lk, [this]() { return (int)queue_data.size() < capacity; });
        queue_data.push(std::move(chunk));
        lk.unlock();
        cv_not_empty.notify_one();
    }
    bool pop(Chunk& out) {
        std::unique_lock<std::mutex> lk(mtx);
        cv_not_empty.wait(lk, [this]() { return finished || !queue_data.empty(); });
        if (queue_data.empty() && finished) return false;
        out = std::move(queue_data.front());
        queue_data.pop();
        lk.unlock();
        cv_not_full.notify_one();
        return true;
    }
    void mark_finished() {
        std::unique_lock<std::mutex> lk(mtx);
        finished = true;
        lk.unlock();
        cv_not_empty.notify_all();
    }
};

struct DecompPipelineContext {
    std::wstring           inPath;
    std::string            outPath;
    uint64_t               chunkSize;
    uint64_t               origSize;
    int                    numChunks;
    BoundedQueue         *readQueue;
    std::atomic<int>       chunksRead;
    std::atomic<int>       chunksDone;
};

static void inflate_chunk_libdeflate(
    const uint8_t *compData,
    size_t compSize,
    uint8_t *outBuf,
    size_t uncompressedSize
) {
    static thread_local libdeflate_decompressor *decomp = nullptr;
    if (!decomp) {
        decomp = libdeflate_alloc_decompressor();
        if (!decomp) throw std::runtime_error("libdeflate_alloc_decompressor() failed");
    }
    size_t actualSize;
    enum libdeflate_result r = libdeflate_zlib_decompress(
        decomp,
        compData,
        compSize,
        outBuf,
        uncompressedSize,
        &actualSize
    );
    if (r != LIBDEFLATE_SUCCESS || actualSize != uncompressedSize) {
        throw std::runtime_error("libdeflate_zlib_decompress failed");
    }
}

void decompress_file_multithread_cb(
    const std::wstring &in_path,
    const std::string &out_path,
    ProgressCallback progress_cb
) {
    HANDLE hHeader = CreateFileW(
        in_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hHeader == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot open MTZ for header: " + std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(in_path));
    }
    uint64_t name_len = 0;
    DWORD bytesRead = 0;
    if (!ReadFile(hHeader, &name_len, sizeof(name_len), &bytesRead, nullptr) || bytesRead != sizeof(name_len)) {
        CloseHandle(hHeader);
        throw std::runtime_error("Failed reading name_len");
    }
    std::vector<char> fnamebuf(name_len);
    if (!ReadFile(hHeader, fnamebuf.data(), static_cast<DWORD>(name_len), &bytesRead, nullptr) || bytesRead != name_len) {
        CloseHandle(hHeader);
        throw std::runtime_error("Failed reading filename");
    }
    std::string original_name(fnamebuf.begin(), fnamebuf.end());
    uint8_t magic_buf[8];
    if (!ReadFile(hHeader, magic_buf, sizeof(MAGIC), &bytesRead, nullptr) || bytesRead != sizeof(MAGIC)) {
        CloseHandle(hHeader);
        throw std::runtime_error("Failed reading magic");
    }
    for (int i = 0; i < 8; i++) if (magic_buf[i] != MAGIC[i]) {
        CloseHandle(hHeader);
        throw std::runtime_error("Invalid MTZ magic");
    }
    uint64_t chunk_size = 0;
    if (!ReadFile(hHeader, &chunk_size, sizeof(chunk_size), &bytesRead, nullptr) || bytesRead != sizeof(chunk_size)) {
        CloseHandle(hHeader);
        throw std::runtime_error("Failed reading chunk_size");
    }
    uint64_t orig_size = 0;
    if (!ReadFile(hHeader, &orig_size, sizeof(orig_size), &bytesRead, nullptr) || bytesRead != sizeof(orig_size)) {
        CloseHandle(hHeader);
        throw std::runtime_error("Failed reading orig_size");
    }
    uint64_t num_chunks = 0;
    if (!ReadFile(hHeader, &num_chunks, sizeof(num_chunks), &bytesRead, nullptr) || bytesRead != sizeof(num_chunks)) {
        CloseHandle(hHeader);
        throw std::runtime_error("Failed reading num_chunks");
    }
    std::vector<uint64_t> comp_sizes(num_chunks);
    for (uint64_t i = 0; i < num_chunks; ++i) {
        if (!ReadFile(hHeader, &comp_sizes[i], sizeof(uint64_t), &bytesRead, nullptr) || bytesRead != sizeof(uint64_t)) {
            CloseHandle(hHeader);
            throw std::runtime_error("Failed reading comp_size");
        }
    }
    uint64_t headerSize = sizeof(name_len) + name_len
                        + sizeof(MAGIC) + 8 + 8 + 8
                        + (8 * num_chunks);
    CloseHandle(hHeader);
    HANDLE hIn = CreateFileW(
        in_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr
    );
    if (hIn == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot open input file (OVERLAPPED): " + std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(in_path));
    }
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw < 1) hw = 2;
    int numWorkers    = hw;
    int queueCapacity = numWorkers * MAX_IN_FLIGHT_FACTOR;
    if (queueCapacity < 1) queueCapacity = 1;
    DecompPipelineContext ctx;
    ctx.inPath    = in_path;
    ctx.outPath   = original_name;
    ctx.chunkSize = chunk_size;
    ctx.origSize  = orig_size;
    ctx.numChunks = static_cast<int>(num_chunks);
    ctx.readQueue = new BoundedQueue(queueCapacity);
    ctx.chunksRead = 0;
    ctx.chunksDone = 0;
    g_totalChunks = static_cast<int>(num_chunks);
    std::thread readerThread([&]() {
        uint64_t offset = headerSize;
        for (uint64_t idx = 0; idx < num_chunks; ++idx) {
            uint64_t csize = comp_sizes[idx];
            std::vector<uint8_t> compBuf(static_cast<size_t>(csize));
            OVERLAPPED ol = {};
            ol.Offset     = static_cast<DWORD>(offset & 0xFFFFFFFF);
            ol.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
            DWORD bytesRd = 0;
            BOOL ok = ReadFile(hIn, compBuf.data(), static_cast<DWORD>(csize), &bytesRd, &ol);
            if (!ok && GetLastError() != ERROR_IO_PENDING) {
                throw std::runtime_error("Overlapped ReadFile failed");
            }
            DWORD transferred = 0;
            BOOL resOk = GetOverlappedResult(hIn, &ol, &transferred, TRUE);
            if (!resOk || transferred != csize) {
                throw std::runtime_error("Overlapped ReadFile got wrong size");
            }
            uint64_t uSize = std::min<uint64_t>(chunk_size, orig_size - idx * chunk_size);
            Chunk chunk{ idx, std::move(compBuf), uSize };
            ctx.readQueue->push(std::move(chunk));
            ctx.chunksRead.fetch_add(1, std::memory_order_relaxed);
            offset += csize;
        }
        ctx.readQueue->mark_finished();
        CloseHandle(hIn);
    });
    std::vector<std::thread> workers;
    workers.reserve(numWorkers);
    for (int w = 0; w < numWorkers; ++w) {
        workers.emplace_back([&, w]() {
            Chunk localChunk;
            while (ctx.readQueue->pop(localChunk)) {
                std::vector<uint8_t> outBuf(static_cast<size_t>(localChunk.uncompressedSize));
                inflate_chunk_libdeflate(
                    localChunk.compData.data(),
                    localChunk.compData.size(),
                    outBuf.data(),
                    static_cast<size_t>(localChunk.uncompressedSize)
                );
                size_t writeOffset = static_cast<size_t>(localChunk.index) * static_cast<size_t>(chunk_size);
                static std::mutex buffer_mutex;
                static std::vector<uint8_t> bigBuffer(static_cast<size_t>(orig_size));
                {
                    std::lock_guard<std::mutex> lg(buffer_mutex);
                    std::memcpy(bigBuffer.data() + writeOffset, outBuf.data(), localChunk.uncompressedSize);
                }
                int progressIndex = static_cast<int>(localChunk.index) + 1;
                ctx.chunksDone.fetch_add(1, std::memory_order_relaxed);
                PostMessageW(g_hMainWnd, WM_APP_PROGRESS, 0, static_cast<LPARAM>(progressIndex));
            }
        });
    }
    readerThread.join();
    for (auto &t : workers) if (t.joinable()) t.join();
    std::vector<uint8_t> finalBuffer(static_cast<size_t>(orig_size));
    {
        std::lock_guard<std::mutex> lg(*new std::mutex());
        memcpy(finalBuffer.data(), finalBuffer.data(), static_cast<size_t>(orig_size)); 
    }
    std::ofstream outStream(ctx.outPath, std::ios::binary | std::ios::trunc);
    if (!outStream) throw std::runtime_error("Cannot open output file: " + ctx.outPath);
    outStream.write(reinterpret_cast<char*>(finalBuffer.data()), static_cast<std::streamsize>(orig_size));
    outStream.close();
    delete ctx.readQueue;
}

static DWORD WINAPI CompressWorker(LPVOID param) {
    auto job = reinterpret_cast<CompressJob*>(param);
    HWND hwnd = job->hwnd;
    auto t1 = std::chrono::high_resolution_clock::now();
    compress_file_singlethread_cb(
        job->inPath,
        job->singleOut,
        job->chunkSizeBytes,
        [hwnd](int chunkIndex) { PostMessageW(hwnd, WM_APP_PROGRESS, 0, static_cast<LPARAM>(chunkIndex)); }
    );
    auto t2 = std::chrono::high_resolution_clock::now();
    double singleTimeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    uint64_t singleOutSize = get_file_size(job->singleOut);
    g_benchGlobal.chunkSizeMiB  = g_benchGlobal.chunkSizeMiB;
    g_benchGlobal.singleTimeMs  = singleTimeMs;
    g_benchGlobal.singleOutSize = singleOutSize;
    g_chunksDone = 0;
    PostMessageW(hwnd, WM_APP_PROGRESS, 0, 0);
    auto t3 = std::chrono::high_resolution_clock::now();
    compress_file_multithread_cb(
        job->inPath,
        job->multiOut,
        job->chunkSizeBytes,
        [hwnd](int chunkIndex) { PostMessageW(hwnd, WM_APP_PROGRESS, 0, static_cast<LPARAM>(chunkIndex)); }
    );
    auto t4 = std::chrono::high_resolution_clock::now();
    double multiTimeMs = std::chrono::duration<double, std::milli>(t4 - t3).count();
    uint64_t multiOutSize = get_file_size(job->multiOut);
    g_benchGlobal.multiTimeMs  = multiTimeMs;
    g_benchGlobal.multiOutSize = multiOutSize;
    PostMessageW(hwnd, WM_APP_MULTI_DONE, 0, 0);
    auto results = new std::wstring;
    {
        std::wostringstream ss;
        ss << L"Compression completed\r\n";
        ss << L"----------------------\r\n";
        ss << L"Chunk size: " << g_benchGlobal.chunkSizeMiB << L" MiB\r\n\r\n";
        ss << L"Single-thread time      : " << std::fixed << std::setprecision(2) << g_benchGlobal.singleTimeMs << L" ms\r\n";
        ss << L"Single-thread file size : " << g_benchGlobal.singleOutSize << L" bytes\r\n\r\n";
        ss << L"Multi-thread time       : " << std::fixed << std::setprecision(2) << g_benchGlobal.multiTimeMs << L" ms\r\n";
        ss << L"Multi-thread file size  : " << g_benchGlobal.multiOutSize << L" bytes\r\n\r\n";
        double speedup = (g_benchGlobal.multiTimeMs > 0.0)
                          ? (g_benchGlobal.singleTimeMs / g_benchGlobal.multiTimeMs)
                          : 0.0;
        ss << L"Speedup (single / multi): " << std::fixed << std::setprecision(2) << speedup << L"\u00D7\r\n";
        *results = ss.str();
    }
    PostMessageW(hwnd, WM_APP_SHOW_RESULTS, 0, reinterpret_cast<LPARAM>(results));
    delete job;
    return 0;
}

static DWORD WINAPI DecompressWorker(LPVOID param) {
    auto job = reinterpret_cast<DecompressJob*>(param);
    HWND hwnd = job->hwnd;
    int requiredSize = MultiByteToWideChar(
        CP_UTF8, 0,
        job->inPath.c_str(), (int)job->inPath.size(),
        nullptr, 0
    );
    std::wstring wInPath(requiredSize, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0,
        job->inPath.c_str(), (int)job->inPath.size(),
        &wInPath[0], requiredSize
    );
    HANDLE hHeader = CreateFileW(
        wInPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hHeader == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd, L"Failed to open MTZ file for reading header.", L"Error", MB_OK);
        delete job;
        return 1;
    }
    uint64_t name_len = 0;
    DWORD bytesRead = 0;
    if (!ReadFile(hHeader, &name_len, sizeof(name_len), &bytesRead, nullptr) || bytesRead != sizeof(name_len)) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Failed reading header", L"Error", MB_OK);
        delete job;
        return 1;
    }
    std::vector<char> fnamebuf(name_len);
    if (!ReadFile(hHeader, fnamebuf.data(), static_cast<DWORD>(name_len), &bytesRead, nullptr) || bytesRead != name_len) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Failed reading filename", L"Error", MB_OK);
        delete job;
        return 1;
    }
    std::string original_name_ansi(fnamebuf.begin(), fnamebuf.end());
    uint8_t magic_buf[8];
    if (!ReadFile(hHeader, magic_buf, sizeof(MAGIC), &bytesRead, nullptr) || bytesRead != sizeof(MAGIC)) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Failed reading magic", L"Error", MB_OK);
        delete job;
        return 1;
    }
    for (int i = 0; i < 8; i++) if (magic_buf[i] != MAGIC[i]) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Invalid MTZ magic", L"Error", MB_OK);
        delete job;
        return 1;
    }
    uint64_t chunk_size = 0;
    if (!ReadFile(hHeader, &chunk_size, sizeof(chunk_size), &bytesRead, nullptr) || bytesRead != sizeof(chunk_size)) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Failed reading chunk_size", L"Error", MB_OK);
        delete job;
        return 1;
    }
    uint64_t orig_size = 0;
    if (!ReadFile(hHeader, &orig_size, sizeof(orig_size), &bytesRead, nullptr) || bytesRead != sizeof(orig_size)) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Failed reading orig_size", L"Error", MB_OK);
        delete job;
        return 1;
    }
    uint64_t num_chunks = 0;
    if (!ReadFile(hHeader, &num_chunks, sizeof(num_chunks), &bytesRead, nullptr) || bytesRead != sizeof(num_chunks)) {
        CloseHandle(hHeader);
        MessageBoxW(hwnd, L"Failed reading num_chunks", L"Error", MB_OK);
        delete job;
        return 1;
    }
    CloseHandle(hHeader);
    g_totalChunks = static_cast<int>(num_chunks);
    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_totalChunks));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
    auto realDecompress = [hwnd, wInPath, original_name_ansi]() {
        decompress_file_multithread_cb(
            wInPath,
            original_name_ansi,
            [hwnd](int chunkIndex) {
                PostMessageW(hwnd, WM_APP_PROGRESS, 0, static_cast<LPARAM>(chunkIndex));
            }
        );
    };
    auto t_start = std::chrono::high_resolution_clock::now();
    try {
        realDecompress();
    } catch (const std::exception &ex) {
        std::wstring msg = L"Decompression failed: ";
        msg += std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(ex.what());
        MessageBoxW(hwnd, msg.c_str(), L"Error", MB_OK);
        delete job;
        return 1;
    }
    auto t_end = std::chrono::high_resolution_clock::now();
    double multiTimeMs = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    uint64_t multiOutSize = get_file_size(original_name_ansi.c_str());
    g_decompGlobal.multiTimeMs  = multiTimeMs;
    g_decompGlobal.multiOutSize = multiOutSize;
    PostMessageW(hwnd, WM_APP_MULTI_DONE, 0, 0);
    auto results = new std::wstring;
    {
        std::wostringstream ss;
        ss << L"Decompression completed\r\n";
        ss << L"------------------------\r\n";
        ss << L"Multi-thread time      : " << std::fixed << std::setprecision(2) << g_decompGlobal.multiTimeMs << L" ms\r\n";
        ss << L"Multi-thread file size : " << g_decompGlobal.multiOutSize << L" bytes\r\n";
        *results = ss.str();
    }
    PostMessageW(hwnd, WM_APP_SHOW_RESULTS, 0, reinterpret_cast<LPARAM>(results));
    delete job;
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;
    InitCommonControlsWrapper();
    const wchar_t CLASS_NAME[] = L"MTZIP_BENCH_GUI";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_hMainWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"MTZIP Benchmark GUI",
        (WS_OVERLAPPEDWINDOW & ~WS_SIZEBOX & ~WS_MAXIMIZEBOX),
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 550,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!g_hMainWnd) return 0;
    ShowWindow(g_hMainWnd, nCmdShow);
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        g_hBtnSelect = CreateWindowExW(
            0, L"BUTTON", L"Select File...",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            20, 20, 120, 30,
            hwnd, reinterpret_cast<HMENU>(ID_BTN_SELECT),
            g_hInst, nullptr
        );
        g_hFilePath = CreateWindowExW(
            0, L"STATIC", L"No file selected.",
            WS_CHILD | WS_VISIBLE,
            160, 20, 500, 30,
            hwnd, reinterpret_cast<HMENU>(ID_FILEPATH),
            g_hInst, nullptr
        );
        CreateWindowExW(
            0, L"STATIC", L"Chunk size (MiB):",
            WS_CHILD | WS_VISIBLE,
            20, 70, 130, 25,
            hwnd, nullptr, g_hInst, nullptr
        );
        g_hEditMib = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"4",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_NUMBER,
            150, 70, 50, 25,
            hwnd, reinterpret_cast<HMENU>(ID_EDIT_CHUNKSIZE),
            g_hInst, nullptr
        );
        g_hBtnCompress = CreateWindowExW(
            0, L"BUTTON", L"Compress",
            WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED,
            220, 70, 100, 30,
            hwnd, reinterpret_cast<HMENU>(ID_BTN_COMPRESS),
            g_hInst, nullptr
        );
        g_hBtnDecompress = CreateWindowExW(
            0, L"BUTTON", L"Decompress",
            WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED,
            340, 70, 120, 30,
            hwnd, reinterpret_cast<HMENU>(ID_BTN_DECOMPRESS),
            g_hInst, nullptr
        );
        g_hProgress = CreateWindowExW(
            0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE,
            20, 120, 660, 25,
            hwnd, reinterpret_cast<HMENU>(ID_PROGRESS),
            g_hInst, nullptr
        );
        SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
        g_hResults = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_READONLY | ES_AUTOVSCROLL | ES_MULTILINE | WS_VSCROLL,
            20, 160, 660, 360,
            hwnd, reinterpret_cast<HMENU>(ID_RESULTS),
            g_hInst, nullptr
        );
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(g_hResults, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == ID_BTN_SELECT) {
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = L"";
            ofn.lStructSize  = sizeof(ofn);
            ofn.hwndOwner    = hwnd;
            ofn.lpstrFile    = szFile;
            ofn.nMaxFile     = MAX_PATH;
            ofn.lpstrFilter  = L"All Files (*.*)\0*.*\0\0";
            ofn.nFilterIndex = 1;
            ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                g_inputPath = szFile;
                SetWindowTextW(g_hFilePath, szFile);
                std::wstring ext = szFile;
                size_t pos = ext.rfind(L'.');
                if (pos != std::wstring::npos && _wcsicmp(ext.substr(pos).c_str(), L".mtz") == 0) {
                    EnableWindow(g_hBtnDecompress, TRUE);
                    EnableWindow(g_hBtnCompress, FALSE);
                } else {
                    EnableWindow(g_hBtnCompress, TRUE);
                    EnableWindow(g_hBtnDecompress, FALSE);
                }
            }
        }
        else if (wmId == ID_BTN_COMPRESS) {
            EnableWindow(g_hBtnSelect, FALSE);
            EnableWindow(g_hEditMib,   FALSE);
            EnableWindow(g_hBtnCompress, FALSE);
            wchar_t buf[16] = {};
            GetWindowTextW(g_hEditMib, buf, 16);
            int mibValue = _wtoi(buf);
            if (mibValue <= 0) mibValue = 4;
            uint64_t chunkSizeBytes = static_cast<uint64_t>(mibValue) * 1024ULL * 1024ULL;
            g_benchGlobal.chunkSizeMiB = mibValue;
            std::wstring wIn = g_inputPath;
            std::wstring base = wIn;
            size_t posDot = base.rfind(L'.');
            if (posDot != std::wstring::npos) base = base.substr(0, posDot);
            std::wstring wOutSingle = base + L"_single_" + std::to_wstring(mibValue) + L"MiB.mtz";
            std::wstring wOutMulti  = base + L"_multi_"  + std::to_wstring(mibValue) + L"MiB.mtz";
            std::string inUtf8        = WideToUtf8(wIn);
            std::string outSingleUtf8 = WideToUtf8(wOutSingle);
            std::string outMultiUtf8  = WideToUtf8(wOutMulti);
            uint64_t origSize = get_file_size(inUtf8);
            g_totalChunks = static_cast<int>((origSize + chunkSizeBytes - 1) / chunkSizeBytes);
            g_chunksDone = 0;
            SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_totalChunks));
            SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_hResults, L"");
            auto job = new CompressJob{ hwnd, inUtf8, outSingleUtf8, outMultiUtf8, chunkSizeBytes };
            CreateThread(nullptr, 0, CompressWorker, job, 0, nullptr);
        }
        else if (wmId == ID_BTN_DECOMPRESS) {
            EnableWindow(g_hBtnSelect, FALSE);
            EnableWindow(g_hBtnDecompress, FALSE);
            EnableWindow(g_hEditMib,   FALSE);
            std::wstring wIn = g_inputPath;
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wIn.c_str(), (int)wIn.size(), nullptr, 0, nullptr, nullptr);
            std::string inUtf8(utf8Len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wIn.c_str(), (int)wIn.size(), &inUtf8[0], utf8Len, nullptr, nullptr);
            auto job = new DecompressJob{ hwnd, inUtf8, "" };
            CreateThread(nullptr, 0, DecompressWorker, job, 0, nullptr);
        }
        break;
    }
    case WM_APP_PROGRESS: {
        int chunkIndex = static_cast<int>(lParam);
        if (chunkIndex <= 0) {
            SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_hFilePath, L"");
        } else {
            SendMessageW(g_hProgress, PBM_SETPOS, chunkIndex, 0);
            std::wstringstream ss;
            ss << L"Progress: Chunk " << chunkIndex << L" / " << g_totalChunks;
            SetWindowTextW(g_hFilePath, ss.str().c_str());
        }
        break;
    }
    case WM_APP_MULTI_DONE: {
        SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
        EnableWindow(g_hBtnSelect, TRUE);
        EnableWindow(g_hEditMib,   TRUE);
        EnableWindow(g_hBtnCompress, TRUE);
        EnableWindow(g_hBtnDecompress, TRUE);
        break;
    }
    case WM_APP_SHOW_RESULTS: {
        auto pResults = reinterpret_cast<std::wstring*>(lParam);
        SetWindowTextW(g_hResults, pResults->c_str());
        delete pResults;
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
