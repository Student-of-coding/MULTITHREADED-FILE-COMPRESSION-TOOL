# MULTITHREADED-FILE-COMPRESSION-TOOL

*COMPANY*: CODTECH IT SOLUTIONS

*NAME*: VAIBHAV SINGH

*INTERN ID*: CT06DK432

*DOMAIN*: C++ Programming

*DURATION*: 6 WEEKS

*MENTOR*: NEELA SANTOSH

## Description

This is a lightweight native Win32 C++ application that provides a graphical interface for compressing and decompressing arbitrary files on Windows. It uses zlib and libdeflate to implement both single-threaded and multithreaded chunked compression, then reports throughput and file-size metrics for comparison. Written with the Win32 API in Visual Studio and compiled via the MSVC toolchain, it demonstrates fundamental file-I/O, overlapping I/O, and multithreading techniques in a GUI setting. Tested on Windows 7/8/10/11.

This tool is not just a compressor, but also a learning aid—helping developers understand the mechanics of chunked compression, synchronization (bounded queues), and progress reporting in a Win32 desktop application.

## Features

* **File Selection** via the native Windows Open File dialog
* **Chunk Size (MiB)** input for flexible compression granularity
* **Single-Threaded Compression** using zlib (`compress2`)
* **Multithreaded Compression** using zlib for each chunk and merging results
* **Multithreaded Decompression** using libdeflate in parallel with overlapped I/O
* **Progress Bar** that updates per-chunk during compress/decompress
* **Results Panel** showing execution time, output file sizes, and speedup factor
* **Fixed-Size GUI** (non-resizable) for simplicity
* **Minimal Dependencies:** only Win32 API, zlib, libdeflate

## Requirements

* Windows OS (7, 8, 10, or 11)
* Visual Studio 2019/2022 (MSVC toolchain)
* zlib development headers and library (`zlib.lib`)
* libdeflate development headers and static library (`deflatestatic.lib`)
* Windows SDK (headers for Win32 API)

## Build Instructions

Clone this repository:

```bash
git clone https://github.com/Student-of-coding/MULTITHREADED-FILE-COMPRESSION-TOOL.git
cd MULTITHREADED-FILE-COMPRESSION-TOOL
```

Ensure the following folders and files exist under the project root:

```text
third_party\zlib\include
third_party\zlib\lib\zlib.lib
third_party\libdeflate\include
third_party\libdeflate\lib\deflatestatic.lib
```

Open a x64 Native Tools Command Prompt for VS 2022 and run:

```bat
cl.exe /EHsc /std:c++17 /Ithird_party\libdeflate\include /Ithird_party\zlib\include ^
    src\mTZIDBench.cpp ^
    third_party\libdeflate\lib\deflatestatic.lib ^
    third_party\zlib\lib\zlib.lib ^
    User32.lib Comctl32.lib Comdlg32.lib Gdi32.lib ^
    /link /MACHINE:X64 /OUT:MTZIPBenchGUI.exe
```

## Usage

Launch `MTZIPBenchGUI.exe`.

1. Click **Select File…** and choose any file (e.g., `.txt`, `.jpg`, `.mp4`, etc.).
2. If the selected file does not end in `.mtz`, the **Compress** button is enabled. Otherwise, **Decompress** is enabled.
3. Enter a chunk size (in **MiB**) into the **"Chunk size (MiB)"** field (default is 4).
4. Click **Compress** to run single-threaded then multithreaded compression. The progress bar updates per chunk; when finished, results appear in the lower panel.
5. Click **Decompress** on any `.mtz` file to restore the original file. The progress bar will update, and timing/size results display once done.

## Sample Performance Results

Below is an example of performance results when compressing a **1 GiB** binary file with a **4 MiB** chunk size. These results were captured on a Windows 11 system (Intel i5-12450H, 16 GB RAM, NVIDIA RTX 3050).

| Metric                         | Single-Threaded | Multithreaded |
| ------------------------------ | --------------- | ------------- |
| Chunk Size (MiB)               | 4               | 4             |
| Execution Time (ms)            | 5384.04         | 967.66        |
| Output File Size (bytes)       | 1 048 118       | 1 048 118     |
| Speedup (Single / Multithread) | —               | 5.56×         |

In this case, multithreaded compression achieved a **5.56×** speedup over the single-threaded baseline, while maintaining identical compressed output size.

## Code Overview

`src/mTZIDBench.cpp` — Full Win32 C++ implementation.

* **`WinMain(...)`** — Registers window class, creates main window, enters message loop.

* **`WndProc(...)`** — Handles window messages, creates controls, processes `WM_COMMAND` for Select/Compress/Decompress, updates progress and results.

* **`compress_file_singlethread_cb(...)`** — Reads input, writes header, compresses each chunk serially with zlib, writes compressed data, then writes header (magic, chunk size, original size, number of chunks, compressed-size table).

* **`compress_file_multithread_cb(...)`** — Same header layout, but spawns threads (limited by hardware concurrency) to compress chunks in parallel. Uses a counting semaphore to bound in-flight jobs and a mutex to serialize writes to the output file.

* **`decompress_file_multithread_cb(...)`** — Opens `.mtz` header via `CreateFileW`, reads the embedded filename, magic, chunk size, original size, and per-chunk compressed sizes. Then spawns one reader thread that issues overlapped `ReadFile` calls to fetch each compressed chunk and pushes them into a bounded queue. Worker threads pop chunks, decompress via libdeflate, and write them into a shared buffer. Finally, writes the fully-assembled original file.

* **`CompressWorker(...)`** and **`DecompressWorker(...)`** — Thread entry points that call the above C-style APIs, record timings, get output file sizes, and post results back to the GUI via custom `WM_APP_` messages.

## Limitations & Future Enhancements

### Known Limitations:

* No encryption or password protection.
* No drag-and-drop support or “Save As” dialog.
* Window is fixed-size and uses basic Win32 controls only.
* No support for pausing/canceling in-flight operations.
* Compression ratio on already-compressed files (e.g., MP4, MP3) will be minimal.

### Possible Enhancements:

* Add support for additional compression codecs (LZ4, Brotli, etc.).
* Implement a **Cancel** button to stop compression/decompression mid-stream.
* Enable drag-and-drop of files onto the main window.
* Support resizable layout and DPI scaling.
* Add real-time charts comparing throughput over time.

---

📌 MTZIPBenchGUI is ideal for developers exploring parallel compression, overlapped I/O, and chunked-stream processing on Windows. It serves as both a practical GUI tool and an educational example for native Win32 multithreaded applications.
