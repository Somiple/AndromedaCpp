#include "midi/export/export_pipeline.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include <immintrin.h>

#include "midi/export/export_encode.h"

namespace andromeda::midi::exporter {

namespace {

// the part of the file that is there from the start is filled through views of one window
// each. the pages of a window are touched, and later filled, in stretches of a chunk: a chunk
// is what one worker touches or copies in one go. larger windows mean fewer mappings and more
// address space and touched pages ahead of the bytes; larger chunks mean fewer hand overs and
// less even work
constexpr std::uint64_t WINDOW_BYTES = 8ull << 20;
constexpr std::uint64_t CHUNK_BYTES = 2ull << 20;
static_assert(WINDOW_BYTES % CHUNK_BYTES == 0 && WINDOW_BYTES % VIEW_ALIGN_BYTES == 0 &&
              CHUNK_BYTES % PAGE_BYTES == 0);

// jobs between being taken and being in the file, for every worker and at least. more keeps
// the workers busy while one slow job holds up the placing; fewer keeps less output in memory
constexpr std::size_t FLIGHT_PER_WORKER = 4;
constexpr std::size_t MIN_IN_FLIGHT = 16;

// one worker in so many may copy into the file, or touch its pages, at a time: the rest keep
// encoding, and page faults on one file do not get faster with more threads
constexpr unsigned WORKERS_PER_COPIER = 2;
constexpr unsigned WORKERS_PER_TOUCHER = 4;

// an output buffer, or a worker's scratch for note offs, that grew past this many usual
// buffer sizes is freed after use instead of kept
constexpr std::size_t KEPT_BUFFER_SIZES = 4;
constexpr std::size_t KEPT_SCRATCH_SIZES = 16;

// a batch gets a buffer this fraction above the least its tracks can take, and a larger one
// when that turns out too small
constexpr std::size_t BATCH_SPARE_FRACTION = 16;

// note offs on their way to a later unit lie in blocks, the smallest piece of that memory
// that is handed on: smaller blocks are free again sooner, larger ones need the pool less
// often. a block holds a power of two of records and no more than a unit has notes, or a
// tiny grain would leave most of every block empty
constexpr std::size_t MAX_BLOCK_RECORDS = 512;
// free blocks a worker keeps to itself so that it rarely needs the pool and its lock
constexpr std::size_t WORKER_BLOCKS = 64;
// a receiver's groups lie all over memory: the one this many ahead is asked for early
constexpr std::size_t PREFETCH_GROUPS = 2;

// an array that is only ever overwritten
template <typename T>
struct Raw {
    std::unique_ptr<T[]> data;
    std::size_t capacity = 0;

    // contents are not kept. a quarter more than asked for, so that it rarely grows again
    [[nodiscard]] T* reserve(std::size_t count) {
        if (count > capacity) {
            data.reset();
            capacity = 0;
            const std::size_t wanted = count + count / 4 + 16;
            data = std::make_unique_for_overwrite<T[]>(wanted);
            capacity = wanted;
        }
        return data.get();
    }

    void trim(std::size_t limit) {
        if (capacity > limit) {
            data.reset();
            capacity = 0;
        }
    }
};

// blocks go round between the units that send note offs ahead and the units that collect them
class BlockPool {
public:
    explicit BlockPool(std::size_t block_records) : block_records_(block_records) {}

    // makes sure the worker's stock holds at least count blocks
    void stock_up(std::vector<std::uint64_t*>& stock, std::size_t count) {
        if (stock.size() >= count) {
            return;
        }
        const std::size_t wanted = count + WORKER_BLOCKS;
        const std::lock_guard lock(mutex_);
        while (stock.size() < wanted && !free_.empty()) {
            stock.push_back(free_.back());
            free_.pop_back();
        }
        if (stock.size() < count) {
            // the blocks that are missing are made in one piece: a track whose note offs all
            // lie far ahead needs new ones by the ten thousand
            const std::size_t made = count - stock.size();
            owned_.push_back(
                std::make_unique_for_overwrite<std::uint64_t[]>(made * block_records_));
            for (std::size_t b = 0; b < made; ++b) {
                stock.push_back(owned_.back().get() + b * block_records_);
            }
        }
    }

    // takes back what a worker's stock holds beyond its share
    void trim(std::vector<std::uint64_t*>& stock) {
        if (stock.size() <= 2 * WORKER_BLOCKS) {
            return;
        }
        const std::lock_guard lock(mutex_);
        free_.insert(free_.end(), stock.begin() + static_cast<std::ptrdiff_t>(WORKER_BLOCKS),
                     stock.end());
        stock.resize(WORKER_BLOCKS);
    }

private:
    const std::size_t block_records_;
    std::mutex mutex_;
    // every block ever made, in the pieces they were made in; they live as long as the pool
    std::vector<std::unique_ptr<std::uint64_t[]>> owned_;
    std::vector<std::uint64_t*> free_;
};

struct AheadRun;

// the note offs one unit sends to one later unit: records [begin, end) of the sender's run,
// in note order. the groups a unit receives form a list
struct AheadGroup {
    AheadRun* run = nullptr;
    std::uint32_t source = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    AheadGroup* next = nullptr;
};

struct AheadBlock {
    std::uint64_t* records = nullptr;
    // groups that still have records in the block
    std::atomic<std::uint32_t> readers{0};
};

// everything one unit sends ahead, group after group. written by the unit's scan before
// any of it is published, then only read; the two counters are what receivers change
struct AheadRun {
    explicit AheadRun(std::size_t block_count) : blocks(block_count) {}

    std::vector<AheadGroup> groups;
    std::vector<AheadBlock> blocks;
    // groups not collected yet
    std::atomic<std::uint32_t> unread{0};
};

// what a worker keeps between jobs
struct Scratch {
    // the note offs of the scanned unit: the records of the ones it emits itself, and for the
    // ones that go ahead their keys and the unit each of them goes to
    Raw<std::uint64_t> local;
    Raw<std::uint64_t> ahead;
    Raw<std::uint32_t> ahead_unit;
    // the note offs of the unit being encoded, and the room their sort needs
    Raw<std::uint64_t> offs;
    Raw<std::uint64_t> spare;
    // counts[k] is for the k-th unit behind the scanned one; zero between scans
    std::vector<std::uint32_t> counts;
    std::vector<std::uint32_t> receivers;
    std::vector<AheadGroup*> carried;
    // free blocks
    std::vector<std::uint64_t*> blocks;
};

struct OutBuffer {
    std::unique_ptr<std::uint8_t[]> data;
    std::size_t capacity = 0;
};

// the bytes of one job, ready for the file once the jobs before it are placed
struct Encoded {
    OutBuffer buffer;
    // the bytes are [begin, begin + size) of the buffer
    std::size_t begin = 0;
    std::size_t size = 0;
    // of a unit with events: its first tick, which its first delta was written as zero
    // against, and its last tick
    std::uint32_t first_tick = 0;
    std::uint32_t last_tick = 0;
    bool has_events = false;
};

// how the encoding of a job ended
enum class Encoding { done, disorder, too_long };

// how the bytes of a chunk get into the file: copied into a window, whose pages were touched
// ahead; copied through a view of their own, behind the part of the file that was there from
// the start, where the file only grows as far as bytes have a place; or with plain writes,
// into a file that cannot be mapped
enum class Way { window, own_view, plain };

// the chunk header of a track that was cut up, written when the length of the track is known
struct TrackHeader {
    std::uint64_t offset = 0;
    std::uint8_t bytes[8] = {};
};

// a block holds this power of two of records: a unit's notes at most
[[nodiscard]] unsigned block_shift_for(std::size_t grain) {
    const std::size_t notes = std::clamp<std::size_t>(grain / 2, 1, MAX_BLOCK_RECORDS);
    return static_cast<unsigned>(std::bit_width(notes) - 1);
}

// workers take jobs in file order. a unit of a cut up track is first scanned, which sends the
// note offs it cannot emit itself to later units, and encoded once every unit before it was
// scanned. a job is encoded into a buffer of its own; its place in the file is known once every
// job before it is encoded, and from then on its bytes wait in a chunk that some worker copies
// into a view of the file. the number of jobs between taking and the end of the copy is
// capped, which caps the memory.
// one mutex guards everything below it in the member list. a worker holds it, through the one
// lock it is born with, only to decide what to do next and to book what it did: scanning,
// encoding, touching and copying run without it. what runs without it works on the plan,
// which nobody changes, on the worker's own scratch and buffer, and on the runs of note offs,
// which have a protocol of their own
class Pipeline {
public:
    Pipeline(const Plan& plan, OutputFile& file, unsigned workers)
        : plan_(plan),
          file_(file),
          reserved_(file.reserved()),
          workers_(workers),
          max_in_flight_(std::max<std::size_t>(FLIGHT_PER_WORKER * workers, MIN_IN_FLIGHT)),
          max_copiers_(std::max(workers / WORKERS_PER_COPIER, 1u)),
          max_touchers_(std::max(workers / WORKERS_PER_TOUCHER, 1u)),
          block_shift_(block_shift_for(plan.grain)),
          block_mask_((std::size_t{1} << block_shift_) - 1),
          blocks_(block_mask_ + 1),
          incoming_(plan.units.size()) {
        slots_.resize(plan.jobs.size());
        runs_.resize(plan.units.size());
    }

    // the views that are still mapped go with it. no thread is left by then
    ~Pipeline() {
        for (const Window& window : windows_) {
            if (window.base != nullptr) {
                OutputFile::unmap(window.base);
            }
        }
    }

    // returns when every worker has ended
    [[nodiscard]] PipelineResult run() {
        {
            std::vector<std::jthread> threads;
            threads.reserve(workers_ - 1);
            try {
                for (unsigned i = 1; i < workers_; ++i) {
                    threads.emplace_back([this] { work(); });
                }
            } catch (const std::exception&) {
                // fewer threads than asked for still do the job
            }
            work();
        }
        if (failed_) {
            return PipelineResult{outcome_, message_, 0};
        }
        return PipelineResult{Outcome::done, "", offset_};
    }

private:
    struct Slot {
        Encoded encoded;
        // chunks that still hold bytes of the job
        std::uint32_t pieces = 0;
        // its notes are checked and the note offs it sends ahead are published
        bool scanned = false;
        // to be handed out for encoding when the frontier passes
        bool waiting = false;
        // encoded
        bool ready = false;
    };

    // a view of the file that encoded bytes are copied into. every first access to a page of
    // a view costs a trip into the kernel, and the kernel serves only a few threads at a time
    // well. so a few workers touch the pages of a window before it is needed, and bytes are
    // copied in only once that is done: the copies are then plain memory copies, and the
    // buffers they come from are free again soon
    struct Window {
        std::uint8_t* base = nullptr;
        // chunks to copy in and stretches still to touch
        std::uint32_t users = 0;
        std::uint32_t untouched = 0;
    };

    // a stretch of a window whose pages are to be touched
    struct Touch {
        std::uint8_t* begin = nullptr;
        std::size_t length = 0;
        std::uint64_t window = 0;
    };

    // pieces that follow each other in the file and go into it in one go, by one worker
    struct Chunk {
        std::vector<Piece> pieces;
        Way way = Way::window;
        // the window they go into, when they go into one
        std::uint64_t window = 0;
    };

    // the body of every worker thread. nothing may leave it: an exception that escapes a
    // thread ends the process
    void work() noexcept {
        Scratch scratch;
        std::unique_lock lock(mutex_);
        try {
            loop(lock, scratch);
        } catch (const std::bad_alloc&) {
            fail_thrown(lock, "out of memory");
        } catch (...) {
            fail_thrown(lock, "export failed");
        }
    }

    // a worker that is thrown out while it holds the mutex may leave what the mutex guards
    // half changed. the failure is booked before the mutex is free again, so that no other
    // worker goes on from that state. messages are literals: this must not need memory
    void fail_thrown(std::unique_lock<std::mutex>& lock, const char* message) noexcept {
        if (!lock.owns_lock()) {
            lock.lock();
        }
        fail_locked(Outcome::failed, message);
    }

    // needs the lock
    void fail_locked(Outcome outcome, const char* message) noexcept {
        if (!failed_) {
            failed_ = true;
            outcome_ = outcome;
            message_ = message;
        }
        wake_.notify_all();
    }

    // needs the lock. input that is not in order, or not yet known to be: an end that
    // overflows needs the end of the track
    void disorder_locked() noexcept {
        fail_locked(plan_.prepared ? Outcome::failed : Outcome::needs_prepare,
                    "the track could not be put in order");
    }

    // needs the lock. work for count more workers has turned up
    void wake(std::size_t count) {
        for (std::size_t i = 0; i < std::min<std::size_t>(count, sleeping_); ++i) {
            wake_.notify_one();
        }
    }

    [[nodiscard]] bool all_done() const {
        return done_jobs_ == plan_.jobs.size() && open_writes_ == 0;
    }

    // what a worker does until everything is done or something failed. writes to the file go
    // first: they free buffers and views. then units that only wait for a worker, then new
    // jobs. a worker sleeps only when none of that is there, and whoever makes work wakes one.
    // the lock is held except where a step says it lets go of it
    void loop(std::unique_lock<std::mutex>& lock, Scratch& scratch) {
        for (;;) {
            if (failed_) {
                return;
            }
            if (header_ready()) {
                write_next_header(lock);
            } else if (!touches_.empty() && touchers_ < max_touchers_) {
                touch_next(lock);
            } else if (copiers_ < max_copiers_ && chunk_ready()) {
                copy_next_chunk(lock);
            } else if (!to_encode_.empty()) {
                const std::uint32_t index = to_encode_.front();
                to_encode_.pop_front();
                encode(index, lock, scratch, nullptr);
            } else if (next_job_ < plan_.jobs.size() && in_flight_ < max_in_flight_) {
                ++in_flight_;
                start(next_job_++, lock, scratch);
            } else if (all_done()) {
                wake_.notify_all();
                return;
            } else if (!filling_.pieces.empty() && copiers_ < max_copiers_) {
                // nothing else to do: do not wait for the chunk to fill up
                close_chunk();
            } else {
                ++sleeping_;
                wake_.wait(lock);
                --sleeping_;
            }
        }
    }

    // needs the lock; releases it while scanning and encoding. a job that was taken is
    // scanned and, when nothing can reach it any more, encoded at once
    void start(std::uint32_t index, std::unique_lock<std::mutex>& lock, Scratch& scratch) {
        const Job& job = plan_.jobs[index];
        const bool segment = job.kind == JobKind::segment;
        ScanResult scan;
        const ScanResult* scanned = nullptr;
        // whole tracks send nothing to other jobs, and a unit without notes has nothing to
        // send: nobody waits for their scan
        if (segment && plan_.units[job.first_unit].on_begin != plan_.units[job.first_unit].on_end) {
            lock.unlock();
            const bool ok = scan_segment(job.first_unit, scratch, scan);
            lock.lock();
            if (!ok) {
                // the unit stays unscanned, so the frontier stops here: no later unit of the
                // track is encoded from notes that may be out of order
                disorder_locked();
                return;
            }
            scanned = &scan;
        }
        slots_[index].scanned = true;
        advance_frontier();
        if (segment && frontier_ <= index) {
            // an earlier unit is still being scanned and may send note offs here; whoever
            // encodes this unit later scans its notes again
            slots_[index].waiting = true;
            return;
        }
        encode(index, lock, scratch, scanned);
    }

    // needs the lock. moves the frontier over every scanned job and hands out the units that
    // waited for it. behind the frontier a unit has received every note off it will get
    void advance_frontier() {
        while (frontier_ < plan_.jobs.size() && slots_[frontier_].scanned) {
            const std::uint32_t index = frontier_++;
            if (slots_[index].waiting) {
                slots_[index].waiting = false;
                to_encode_.push_back(index);
                wake(1);
            }
        }
    }

    // needs the lock; releases it while encoding. scanned is the result of this worker's
    // scan of the job when the unit's own note offs are still in the scratch
    void encode(std::uint32_t index, std::unique_lock<std::mutex>& lock, Scratch& scratch,
                const ScanResult* scanned) {
        lock.unlock();
        Encoded encoded;
        Encoding result = Encoding::done;
        switch (plan_.jobs[index].kind) {
        case JobKind::prelude:
            encode_prelude(index, encoded);
            break;
        case JobKind::batch:
            result = encode_batch(index, scratch, encoded);
            break;
        case JobKind::segment:
            result = encode_segment(index, scratch, scanned, encoded);
            break;
        }
        lock.lock();
        switch (result) {
        case Encoding::done:
            slots_[index].encoded = std::move(encoded);
            slots_[index].ready = true;
            place_ready();
            break;
        case Encoding::disorder:
            disorder_locked();
            break;
        case Encoding::too_long:
            fail_locked(Outcome::failed, "track length overflow");
            break;
        }
    }

    // needs the lock. gives every job that now has a known place its offset
    void place_ready() {
        while (placed_jobs_ < plan_.jobs.size() && slots_[placed_jobs_].ready && !failed_) {
            // counted first: a job is placed once, whatever happens while it is
            place(placed_jobs_++);
        }
        if (!filling_.pieces.empty()) {
            if (placed_jobs_ == plan_.jobs.size() || offset_ >= filling_end_) {
                close_chunk();
            } else if (chunks_.empty()) {
                // a sleeping worker may as well copy what is there
                wake(1);
            }
        }
        if (all_done()) {
            wake_.notify_all();
        }
    }

    // the end of the stretch of the file that goes into it in one go with the byte at the
    // offset: a chunk sized stretch, cut short where the windows end
    [[nodiscard]] std::uint64_t chunk_end(std::uint64_t offset) const {
        const std::uint64_t end = (offset / CHUNK_BYTES + 1) * CHUNK_BYTES;
        return offset < reserved_ ? std::min(end, reserved_) : end;
    }

    // needs the lock. the job gets the bytes of the file behind the job before it: its bytes
    // are cut into pieces, one for every chunk they reach into
    void place(std::uint32_t index) {
        Slot& slot = slots_[index];
        Encoded& encoded = slot.encoded;
        const Job& job = plan_.jobs[index];
        const Unit* const unit =
            job.kind == JobKind::segment ? &plan_.units[job.first_unit] : nullptr;
        if (unit != nullptr) {
            if (job.first_unit == plan_.track_units[unit->track]) {
                // room for the chunk header of the track
                track_offset_ = offset_;
                offset_ += 8;
                track_bytes_ = 0;
                track_tick_ = 0;
            }
            if (encoded.has_events) {
                // the unit was encoded as if nothing came before it: its first delta is a zero
                // byte, with room in front of it for the longest delta there is
                const std::uint32_t delta = encoded.first_tick - track_tick_;
                const std::size_t extra = vlq_length(delta) - 1;
                encoded.begin -= extra;
                encoded.size += extra;
                put_vlq(encoded.buffer.data.get() + encoded.begin, delta);
                track_tick_ = encoded.last_tick;
            }
            track_bytes_ += encoded.size;
        }

        const std::uint8_t* source = encoded.buffer.data.get() + encoded.begin;
        std::size_t left = encoded.size;
        if (left == 0) {
            recycle(index);
        }
        while (left > 0) {
            if (!filling_.pieces.empty() && offset_ >= filling_end_) {
                close_chunk();
            }
            if (filling_.pieces.empty()) {
                filling_end_ = chunk_end(offset_);
                if (offset_ < reserved_) {
                    // the window the bytes go into, and the next one so that its pages are
                    // there in time
                    const std::uint64_t window = offset_ / WINDOW_BYTES;
                    open_window(window);
                    open_window(window + 1);
                    if (failed_) {
                        return;
                    }
                    filling_.way = Way::window;
                    filling_.window = window;
                    ++window_at(window).users;
                } else {
                    filling_.way = reserved_ != 0 ? Way::own_view : Way::plain;
                }
            }
            const std::size_t part =
                static_cast<std::size_t>(std::min<std::uint64_t>(left, filling_end_ - offset_));
            filling_.pieces.push_back(Piece{source, offset_, part, index});
            ++slot.pieces;
            source += part;
            left -= part;
            offset_ += part;
        }

        if (unit != nullptr && unit->last) {
            // 64 bits up to here: the length is only cut down once it is known to fit
            if (track_bytes_ > 0xFFFFFFFFull) {
                fail_locked(Outcome::failed, "track length overflow");
                return;
            }
            TrackHeader header;
            header.offset = track_offset_;
            std::memcpy(header.bytes, "MTrk", 4);
            store_be32(header.bytes + 4, static_cast<std::uint32_t>(track_bytes_));
            headers_.push_back(header);
            ++open_writes_;
            wake(1);
        }
    }

    [[nodiscard]] Window& window_at(std::uint64_t window) {
        return windows_[static_cast<std::size_t>(window - first_window_)];
    }

    // needs the lock. maps a window, unless it lies behind the part of the file that is
    // there, and has its pages touched
    void open_window(std::uint64_t window) {
        while (first_window_ + windows_.size() <= window) {
            windows_.emplace_back();
        }
        Window& opened = window_at(window);
        const std::uint64_t begin = window * WINDOW_BYTES;
        if (opened.base != nullptr || begin >= reserved_ || failed_) {
            return;
        }
        const std::uint64_t end = std::min(begin + WINDOW_BYTES, reserved_);
        opened.base = file_.map(placed_section(end), begin, end);
        if (opened.base == nullptr) {
            fail_locked(Outcome::file_failed, "could not write the file");
            return;
        }
        for (std::uint64_t at = begin; at < end; at += CHUNK_BYTES) {
            ++opened.users;
            ++opened.untouched;
            touches_.push_back(Touch{opened.base + (at - begin),
                                     static_cast<std::size_t>(std::min(CHUNK_BYTES, end - at)),
                                     window});
            wake(1);
        }
    }

    // needs the lock. returns the view of a window nothing will use any more, to be unmapped
    // without the lock
    [[nodiscard]] std::uint8_t* leave_window(std::uint64_t window) {
        --window_at(window).users;
        // windows are left roughly in order: only the oldest ones are looked at
        std::uint8_t* retired = nullptr;
        while (retired == nullptr && !windows_.empty() && windows_.front().users == 0 &&
               (first_window_ + 1) * WINDOW_BYTES <= offset_) {
            retired = windows_.front().base;
            windows_.pop_front();
            ++first_window_;
        }
        return retired;
    }

    // needs the lock. the chunk being filled goes to the workers
    void close_chunk() {
        chunks_.push_back(std::move(filling_));
        filling_ = Chunk{};
        ++open_writes_;
        wake(1);
    }

    // needs the lock; releases it while touching
    void touch_next(std::unique_lock<std::mutex>& lock) {
        const Touch touch = touches_.front();
        touches_.pop_front();
        ++touchers_;
        lock.unlock();
        const bool ok = touch_guarded(touch.begin, touch.length);
        lock.lock();
        --touchers_;
        if (!ok) {
            fail_locked(Outcome::file_failed, "could not write the file");
        }
        --window_at(touch.window).untouched;
        finish_write(lock, leave_window(touch.window), 0);
    }

    // needs the lock. bytes are copied into a window only when its pages have been touched.
    // plain writes go one at a time, in file order: the file then grows without gaps
    [[nodiscard]] bool chunk_ready() {
        if (chunks_.empty()) {
            return false;
        }
        const Chunk& chunk = chunks_.front();
        switch (chunk.way) {
        case Way::window:
            return window_at(chunk.window).untouched == 0;
        case Way::own_view:
            return true;
        case Way::plain:
            break;
        }
        return !plain_write_;
    }

    // needs the lock. a mapping that covers the file up to end, which lies in the part that
    // was there from the start or in bytes that have their place: when the file has to grow
    // for it, it grows to where the placed bytes end, never further
    [[nodiscard]] void* placed_section(std::uint64_t end) {
        return file_.section_for(end, offset_);
    }

    // needs the lock; releases it while copying or writing
    void copy_next_chunk(std::unique_lock<std::mutex>& lock) {
        const Chunk chunk = std::move(chunks_.front());
        chunks_.pop_front();
        const Piece* const pieces = chunk.pieces.data();
        const std::size_t count = chunk.pieces.size();
        bool ok = true;
        ++copiers_;
        if (chunk.way == Way::window) {
            std::uint8_t* const view = window_at(chunk.window).base;
            lock.unlock();
            ok = copy_guarded(pieces, count, view, chunk.window * WINDOW_BYTES);
            lock.lock();
        } else if (chunk.way == Way::own_view) {
            void* const section =
                placed_section(pieces[count - 1].offset + pieces[count - 1].length);
            if (section != nullptr) {
                lock.unlock();
                ok = file_.write_through(section, pieces, count);
                lock.lock();
            } else {
                ok = false;
            }
        } else {
            plain_write_ = true;
            lock.unlock();
            ok = file_.write(pieces, count);
            lock.lock();
            plain_write_ = false;
        }
        --copiers_;
        if (!ok) {
            fail_locked(Outcome::file_failed, "could not write the file");
        }
        for (const Piece& piece : chunk.pieces) {
            if (--slots_[piece.job].pieces == 0) {
                recycle(piece.job);
            }
        }
        finish_write(lock, chunk.way == Way::window ? leave_window(chunk.window) : nullptr, 1);
    }

    // needs the lock. a chunk header is written only once the windows it lies in are touched
    // or gone: touching must not meet another write to its bytes. those windows were opened
    // when the bytes before the header were placed, so one that is not in the list is gone,
    // or the header lies behind the windows
    [[nodiscard]] bool header_ready() const {
        if (headers_.empty()) {
            return false;
        }
        const std::uint64_t offset = headers_.front().offset;
        for (std::uint64_t window = offset / WINDOW_BYTES; window <= (offset + 7) / WINDOW_BYTES;
             ++window) {
            if (window >= first_window_ && window - first_window_ < windows_.size() &&
                windows_[static_cast<std::size_t>(window - first_window_)].untouched != 0) {
                return false;
            }
        }
        return true;
    }

    // needs the lock; releases it while writing. the header goes through a small view of its
    // own: the window it lies in may be gone by now
    void write_next_header(std::unique_lock<std::mutex>& lock) {
        const TrackHeader header = headers_.front();
        headers_.pop_front();
        const Piece piece{header.bytes, header.offset, sizeof(header.bytes), 0};
        bool ok = true;
        if (reserved_ == 0) {
            lock.unlock();
            ok = file_.write(&piece, 1);
            lock.lock();
        } else if (void* const section = placed_section(piece.offset + piece.length)) {
            lock.unlock();
            ok = file_.write_through(section, &piece, 1);
            lock.lock();
        } else {
            ok = false;
        }
        if (!ok) {
            fail_locked(Outcome::file_failed, "could not write the file");
        }
        finish_write(lock, nullptr, 1);
    }

    // needs the lock. the common end of everything that writes to the file: counts the write
    // as done, lets sleeping workers know what they can do now and unmaps a view that is no
    // longer used, without the lock
    void finish_write(std::unique_lock<std::mutex>& lock, std::uint8_t* retired,
                      std::size_t writes_done) {
        open_writes_ -= writes_done;
        wake((touches_.empty() ? 0 : 1) + (chunk_ready() ? 1 : 0) + (header_ready() ? 1 : 0));
        if (all_done()) {
            wake_.notify_all();
        }
        if (retired != nullptr) {
            lock.unlock();
            OutputFile::unmap(retired);
            lock.lock();
        }
    }

    // needs the lock. the job is finished and its buffer free again
    void recycle(std::uint32_t index) {
        OutBuffer& buffer = slots_[index].encoded.buffer;
        if (buffer.data != nullptr && buffer.capacity <= KEPT_BUFFER_SIZES * plan_.buffer_bytes) {
            buffers_.push_back(std::move(buffer));
        }
        buffer = OutBuffer{};
        // room for a worker that waits to take the next job
        wake(in_flight_ == max_in_flight_ && next_job_ < plan_.jobs.size() ? 1 : 0);
        --in_flight_;
        ++done_jobs_;
    }

    // for a worker that does not hold the lock
    [[nodiscard]] OutBuffer take_buffer(std::size_t capacity) {
        OutBuffer buffer;
        {
            const std::lock_guard lock(mutex_);
            if (!buffers_.empty()) {
                buffer = std::move(buffers_.back());
                buffers_.pop_back();
            }
        }
        if (buffer.capacity < capacity) {
            buffer.data.reset();
            buffer.capacity = std::max(capacity, plan_.buffer_bytes);
            buffer.data = std::make_unique_for_overwrite<std::uint8_t[]>(buffer.capacity);
        }
        return buffer;
    }

    // the encoders run without the lock and leave the bytes of the job in encoded

    void encode_prelude(std::uint32_t index, Encoded& encoded) {
        encoded.buffer = take_buffer(plan_.jobs[index].bytes + 8);
        std::uint8_t* const begin = encoded.buffer.data.get();
        encoded.size = static_cast<std::size_t>(put_prelude(plan_, begin) - begin);
    }

    // the note offs of a unit in tick order, note order kept among equal ticks. the array to
    // sort into is only asked for when they need sorting: for a unit with many note offs on
    // one tick it would be large
    [[nodiscard]] static const std::uint64_t* sorted_offs(std::uint64_t* records, std::size_t count,
                                                          Scratch& scratch) {
        if (in_tick_order(records, count)) {
            return records;
        }
        return sort_by_tick(records, scratch.spare.reserve(count), count);
    }

    // every unit of the job is a whole track with its chunk header
    [[nodiscard]] Encoding encode_batch(std::uint32_t index, Scratch& scratch, Encoded& encoded) {
        const Job& job = plan_.jobs[index];
        encoded.buffer = take_buffer(job.bytes + job.bytes / BATCH_SPARE_FRACTION + MERGE_SLACK);
        std::size_t used = 0;
        for (std::uint32_t u = job.first_unit; u < job.first_unit + job.unit_count; ++u) {
            const Unit& unit = plan_.units[u];
            const TrackView& view = plan_.tracks[unit.track];
            const std::size_t ons = unit.on_end - unit.on_begin;
            std::uint64_t* const local = scratch.local.reserve(ons + 1);
            const ScanResult scan = scan_notes(view, unit, local, scratch.ahead.reserve(ons + 1));
            if (!scan.ordered || (scan.overflow && !plan_.prepared) ||
                !channel_in_order(view, unit)) {
                return Encoding::disorder;
            }
            const std::uint64_t* const offs = sorted_offs(local, scan.local, scratch);

            // the chunk header, the events with a first delta of up to five bytes, the end of track
            const std::size_t room =
                8 + measure(view, unit, offs, scan.local).bytes + 4 + 4 + MERGE_SLACK;
            if (used + room > encoded.buffer.capacity) {
                // the bound is above the plan's guess: it allows for long deltas, and for every
                // note to end where it starts on top of the note offs that are there
                OutBuffer grown;
                grown.capacity = 2 * (used + room);
                grown.data = std::make_unique_for_overwrite<std::uint8_t[]>(grown.capacity);
                std::memcpy(grown.data.get(), encoded.buffer.data.get(), used);
                encoded.buffer = std::move(grown);
            }
            std::uint8_t* const header = encoded.buffer.data.get() + used;
            const Cursor end = merge_events(Cursor{header + 8, 0, view.channel + unit.ch_begin},
                                            view, unit, offs, scan.local);
            store32(end.out, END_OF_TRACK);
            // 64 bits up to here: the length is only cut down once it is known to fit
            const std::uint64_t bytes = static_cast<std::uint64_t>(end.out + 4 - header) - 8;
            if (bytes > 0xFFFFFFFFull) {
                return Encoding::too_long;
            }
            std::memcpy(header, "MTrk", 4);
            store_be32(header + 4, static_cast<std::uint32_t>(bytes));
            used += static_cast<std::size_t>(bytes) + 8;
        }
        encoded.size = used;
        return Encoding::done;
    }

    // scans the notes of a unit: the note offs it emits itself stay in the scratch, the others
    // are published for the units that emit them. false when the input is not usable as it is
    [[nodiscard]] bool scan_segment(std::uint32_t unit_index, Scratch& scratch, ScanResult& scan) {
        const Unit& unit = plan_.units[unit_index];
        const TrackView& view = plan_.tracks[unit.track];
        const std::size_t ons = unit.on_end - unit.on_begin;
        scan =
            scan_notes(view, unit, scratch.local.reserve(ons + 1), scratch.ahead.reserve(ons + 1));
        if (!scan.ordered || (scan.overflow && !plan_.prepared)) {
            return false;
        }
        if (scan.ahead == 0) {
            return true;
        }

        const std::size_t count = scan.ahead;
        const std::uint32_t first = unit_index + 1;
        const Route& route = plan_.routes[unit.track];
        const std::uint64_t* const ahead = scratch.ahead.data.get();
        std::uint32_t* const target = scratch.ahead_unit.reserve(count);
        std::uint32_t farthest = first;
        std::uint32_t to = first;
        for (std::size_t i = 0; i < count; ++i) {
            // never below first for input in order; the clamp keeps unsorted input harmless
            to = std::max(first, route.unit_for(plan_.units.data(), ahead[i], to));
            target[i] = to;
            farthest = std::max(farthest, to);
        }
        // counts[k] belongs to unit first + k. the array only reaches as far as the note offs
        // go: every worker has one, and a track can have a great many units
        std::vector<std::uint32_t>& counts = scratch.counts;
        std::vector<std::uint32_t>& receivers = scratch.receivers;
        if (counts.size() <= farthest - first) {
            counts.resize(static_cast<std::size_t>(farthest - first) + 1, 0);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (counts[target[i] - first]++ == 0) {
                receivers.push_back(target[i]);
            }
        }
        std::sort(receivers.begin(), receivers.end());

        // the records go into blocks group by group, the counts turning into write positions
        const std::size_t block_count = (count + block_mask_) >> block_shift_;
        auto run = std::make_unique<AheadRun>(block_count);
        blocks_.stock_up(scratch.blocks, block_count);
        AheadBlock* const blocks = run->blocks.data();
        for (std::size_t b = 0; b < block_count; ++b) {
            blocks[b].records = scratch.blocks.back();
            scratch.blocks.pop_back();
        }
        run->groups.reserve(receivers.size());
        std::uint32_t total = 0;
        for (const std::uint32_t receiver : receivers) {
            const std::uint32_t n = counts[receiver - first];
            counts[receiver - first] = total;
            run->groups.push_back(AheadGroup{run.get(), unit_index, total, total + n, nullptr});
            for (std::size_t b = total >> block_shift_; b <= (total + n - 1) >> block_shift_; ++b) {
                blocks[b].readers.fetch_add(1, std::memory_order_relaxed);
            }
            total += n;
        }
        const unsigned shift = block_shift_;
        const std::size_t mask = block_mask_;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t at = counts[target[i] - first]++;
            blocks[at >> shift].records[at & mask] = off_record(view, ahead[i]);
        }

        // publish. the run is complete before the first group is linked in, and a receiver
        // reads its list only once the frontier has passed this unit
        run->unread.store(static_cast<std::uint32_t>(run->groups.size()),
                          std::memory_order_relaxed);
        AheadRun* const published = run.get();
        runs_[unit_index] = std::move(run);
        for (std::size_t g = 0; g < receivers.size(); ++g) {
            AheadGroup& group = published->groups[g];
            std::atomic<AheadGroup*>& list = incoming_[receivers[g]];
            group.next = list.load(std::memory_order_relaxed);
            while (!list.compare_exchange_weak(group.next, &group, std::memory_order_release,
                                               std::memory_order_relaxed)) {
            }
            counts[receivers[g] - first] = 0;
        }
        receivers.clear();
        return true;
    }

    // moves the note offs earlier units sent to this unit into the scratch, in note order, with
    // room behind them for room more records. returns their count. once per unit, behind the
    // frontier: the groups are gone afterwards
    [[nodiscard]] std::size_t collect_carried(std::uint32_t unit_index, Scratch& scratch,
                                              std::size_t room) {
        std::vector<AheadGroup*>& carried = scratch.carried;
        carried.clear();
        std::size_t total = 0;
        for (AheadGroup* group = incoming_[unit_index].load(std::memory_order_acquire);
             group != nullptr; group = group->next) {
            carried.push_back(group);
            total += group->end - group->begin;
        }
        std::sort(carried.begin(), carried.end(),
                  [](const AheadGroup* a, const AheadGroup* b) { return a->source < b->source; });

        std::uint64_t* out = scratch.offs.reserve(total + room);
        const unsigned shift = block_shift_;
        const std::size_t mask = block_mask_;
        const auto first_record = [&](const AheadGroup& group) {
            return group.run->blocks[group.begin >> shift].records + (group.begin & mask);
        };
        for (std::size_t c = 0; c < carried.size(); ++c) {
            if (c + PREFETCH_GROUPS < carried.size()) {
                _mm_prefetch(
                    reinterpret_cast<const char*>(first_record(*carried[c + PREFETCH_GROUPS])),
                    _MM_HINT_T0);
            }
            const AheadGroup& group = *carried[c];
            AheadRun& run = *group.run;
            const std::uint32_t source = group.source;
            const std::size_t end = group.end;
            for (std::size_t at = group.begin; at < end;) {
                AheadBlock& block = run.blocks[at >> shift];
                const std::size_t inside = at & mask;
                const std::size_t part = std::min<std::size_t>(end - at, mask + 1 - inside);
                std::memcpy(out, block.records + inside, part * sizeof(std::uint64_t));
                out += part;
                at += part;
                // the last group to read a block keeps it for this worker's next scan
                if (block.readers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    scratch.blocks.push_back(block.records);
                }
            }
            // the run goes with its last group; the group itself is part of it
            if (run.unread.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                runs_[source].reset();
            }
        }
        blocks_.trim(scratch.blocks);
        return total;
    }

    // encodes a unit; every earlier unit has been scanned. scanned is the result of this
    // worker's scan when the unit's own note offs are still in the scratch
    [[nodiscard]] Encoding encode_segment(std::uint32_t index, Scratch& scratch,
                                          const ScanResult* scanned, Encoded& encoded) {
        const std::uint32_t unit_index = plan_.jobs[index].first_unit;
        const Unit& unit = plan_.units[unit_index];
        const TrackView& view = plan_.tracks[unit.track];
        if (!channel_in_order(view, unit)) {
            return Encoding::disorder;
        }

        // the note offs of earlier notes first, then the unit's own: that is note order, and
        // the sort keeps it among equal ticks
        const std::size_t ons = unit.on_end - unit.on_begin;
        const std::size_t carried = collect_carried(unit_index, scratch, ons + 1);
        std::uint64_t* const records = scratch.offs.data.get();
        std::size_t off_count = carried;
        if (scanned != nullptr) {
            std::memcpy(records + carried, scratch.local.data.get(),
                        scanned->local * sizeof(std::uint64_t));
            off_count += scanned->local;
        } else {
            off_count +=
                scan_notes(view, unit, records + carried, scratch.ahead.reserve(ons + 1)).local;
        }
        const std::uint64_t* const offs = sorted_offs(records, off_count, scratch);

        encoded.has_events =
            unit.on_begin != unit.on_end || off_count > 0 || unit.ch_begin != unit.ch_end;
        if (encoded.has_events || unit.last) {
            const Extent extent = measure(view, unit, offs, off_count);
            encoded.buffer = take_buffer(4 + extent.bytes + 4 + MERGE_SLACK);
            // room in front for the real first delta
            encoded.begin = 4;
            std::uint8_t* const begin = encoded.buffer.data.get() + encoded.begin;
            Cursor end = merge_events(
                Cursor{begin, encoded.has_events ? extent.first : 0, view.channel + unit.ch_begin},
                view, unit, offs, off_count);
            encoded.first_tick = extent.first;
            encoded.last_tick = end.tick;
            if (unit.last) {
                store32(end.out, END_OF_TRACK);
                end.out += 4;
            }
            encoded.size = static_cast<std::size_t>(end.out - begin);
        }
        // a unit that received far more note offs than usual: do not sit on the room they took
        const std::size_t kept = KEPT_SCRATCH_SIZES * plan_.buffer_bytes / sizeof(std::uint64_t);
        scratch.offs.trim(kept);
        scratch.spare.trim(kept);
        return Encoding::done;
    }

    const Plan& plan_;
    // its methods for any thread are used without the mutex, section_for with it
    OutputFile& file_;
    // the file is there up to here from the start, with windows over it; zero for a file
    // that takes plain writes
    const std::uint64_t reserved_;
    const unsigned workers_;
    const std::size_t max_in_flight_;
    const unsigned max_copiers_;
    const unsigned max_touchers_;

    // a block holds 1 << block_shift_ records
    const unsigned block_shift_;
    const std::size_t block_mask_;
    BlockPool blocks_;
    // what a unit sends ahead: set by its scan, reset by the last receiver that collects from
    // it. the two never overlap, the frontier lies between them
    std::vector<std::unique_ptr<AheadRun>> runs_;
    // what a unit receives: pushed to by the scans of earlier units, complete once the
    // frontier has passed the unit and read only then
    std::vector<std::atomic<AheadGroup*>> incoming_;

    // guards everything below
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Slot> slots_;
    std::vector<OutBuffer> buffers_;
    // scanned units that can be encoded now and that no worker holds
    std::deque<std::uint32_t> to_encode_;
    // bytes with a place in the file: the chunk still being filled, which ends at
    // filling_end_ in the file, and the ones to copy, in file order
    Chunk filling_;
    std::uint64_t filling_end_ = 0;
    std::deque<Chunk> chunks_;
    std::deque<TrackHeader> headers_;
    // views of the file, windows_[0] being window first_window_, and stretches of them to touch
    std::deque<Window> windows_;
    std::uint64_t first_window_ = 0;
    std::deque<Touch> touches_;
    std::uint32_t next_job_ = 0;
    // every job below has been scanned
    std::uint32_t frontier_ = 0;
    // every job below has its place in the file
    std::uint32_t placed_jobs_ = 0;
    // jobs that were taken and are not in the file yet
    std::size_t in_flight_ = 0;
    std::size_t done_jobs_ = 0;
    // chunks and track headers on their way into the file
    std::size_t open_writes_ = 0;
    std::size_t sleeping_ = 0;
    unsigned copiers_ = 0;
    unsigned touchers_ = 0;
    // a worker is in a plain write of a chunk
    bool plain_write_ = false;
    std::uint64_t offset_ = 0;
    std::uint64_t track_offset_ = 0;
    std::uint64_t track_bytes_ = 0;
    std::uint32_t track_tick_ = 0;
    bool failed_ = false;
    Outcome outcome_ = Outcome::done;
    const char* message_ = "";
};

}

PipelineResult run_pipeline(const Plan& plan, OutputFile& file, unsigned workers) noexcept {
    try {
        Pipeline pipeline(plan, file, std::max(workers, 1u));
        return pipeline.run();
    } catch (const std::bad_alloc&) {
        return PipelineResult{Outcome::failed, "out of memory", 0};
    } catch (...) {
        return PipelineResult{Outcome::failed, "export failed", 0};
    }
}

}
