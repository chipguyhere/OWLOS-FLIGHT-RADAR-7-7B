#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ThreadsafeWire wraps a TwoWire instance with a recursive FreeRTOS mutex, so
// multiple tasks can safely share one I2C bus.
//
// It lives in its own header, separate from any board class, so that device
// drivers can be written against it without dragging in a whole board. A driver
// that takes a ThreadsafeWire& instead of a TwoWire& is usable on a shared bus
// by construction; see the ADS1115 and MCP4728 drivers in chipguy_HKL-ER6_board
// for the shape.
//
// This is the canonical copy. Board libraries VENDOR it — copy both files into
// src/ — rather than depending on chipguy_Board, so a published board library
// needs nothing but Wire. Copy it forward; do not fork it.
//
// LOCKING IS AUTOMATIC. You do not call lock().
//
// The unit of locking is the TRANSACTION, not the method call:
// beginTransmission() takes the mutex and holds it until endTransmission(true),
// or until the requestFrom() that completes a repeated start. A stand-alone
// requestFrom() locks for its own duration. So ordinary code --
//
//     wire.beginTransmission(addr);
//     wire.write(reg);
//     wire.endTransmission();
//
// — is already atomic against every other task. Existing code ported from
// TwoWire is correct simply by changing the type, which is the whole idea.
//
// (Per-method locking would be actively dangerous, not merely insufficient: the
// ESP32's own TwoWire holds an internal HAL semaphore from beginTransmission()
// to endTransmission(true). A task that took our mutex in the middle of another
// task's transaction would block on that HAL lock while holding ours, and the
// other task would block on ours while holding the HAL lock — AB-BA deadlock.
// Holding our mutex across the whole transaction makes the two always nest in
// the same order.)
//
// lock()/unlock() and the RAII Guard remain public for the one case the above
// does not cover: making SEVERAL transactions atomic as a group — a read-modify-
// write, or a device whose register protocol spans more than one transaction.
// The mutex is recursive, so the automatic per-transaction takes nest harmlessly
// inside an outer Guard.
//
// A read is the common case that DOES need the outer Guard: requestFrom() leaves
// the bytes in TwoWire's single shared RX buffer, so the read() calls that drain
// it must be covered by the same Guard as the requestFrom() that filled it, or
// another task's transaction can clobber the buffer in between. The readRegister
// / readByte helpers below do this for you.
//
// TWO PRIORITIES
//
// Sooner or later one device on a shared bus moves far more data than the rest.
// A 128x64 OLED is a kilobyte per frame, which at 100 kHz is on the order of a
// hundred milliseconds. Anything else that wants the bus — a sensor, a relay
// write — would have to wait behind it, and a hundred-millisecond stall to read
// a sensor is not acceptable.
//
// So the bus has two classes of user:
//
//   lock()/unlock(), Guard             — normal priority. The default. Callers
//                                        register demand for the bus, which a
//                                        background streamer can see.
//   lockBackground()/unlockBackground(),
//   BackgroundGuard                    — bulk transfers that should get out of
//                                        the way. Acquires the same mutex but
//                                        does NOT register demand.
//
// A background streamer holds the bus only for one small chunk at a time, and
// between chunks calls isWanted() and, if anything is waiting, yieldToWaiters().
// Worst-case latency for a normal-priority caller is therefore one chunk, not
// one whole transfer.
//
// This is cooperative: nothing forces a background user to check. It works
// because there is normally exactly one background user and it is written to
// check.

// ── Stall behaviour, tunable per sketch with -D ────────────────────────────
//
// A bus that stops moving is always a bug — a transaction begun and never
// ended, a repeated start left open, a Guard held across something slow, a task
// that died mid-transaction. The tuning here is chosen to make that bug easy to
// find rather than easy to ignore:
//
//   * Acquisition STALLS SILENTLY for STALL_WARN_MS first. Several seconds of a
//     visibly frozen device is the most reliable bug report there is — somebody
//     notices. Failing fast would just produce a device that misbehaves subtly
//     and intermittently, which is much harder to chase.
//   * Then it becomes very loud on Serial: who is waiting, who holds the bus,
//     for how long, and what usually causes it. Repeated while it lasts.
//   * At STALL_GIVEUP_MS it gives up. The transaction then FAILS CLEANLY
//     (endTransmission returns ERR_BUS_STALLED, write/requestFrom return 0)
//     rather than touching the bus without the mutex — proceeding unlocked
//     would corrupt whatever the holder is doing, and would block on the
//     platform's own HAL lock anyway.
//
// Same philosophy on the other side: a background streamer standing aside for a
// normal-priority caller waits a long time before resuming, because a stalled
// bulk transfer is the visible symptom that leads someone to the sloppy code
// holding the bus. Anything over HOG_WARN_MS gets reported.
#ifndef CHIPGUY_TSWIRE_STALL_WARN_MS
#define CHIPGUY_TSWIRE_STALL_WARN_MS 3000
#endif
#ifndef CHIPGUY_TSWIRE_STALL_GIVEUP_MS
#define CHIPGUY_TSWIRE_STALL_GIVEUP_MS 15000
#endif
#ifndef CHIPGUY_TSWIRE_HOG_WARN_MS
#define CHIPGUY_TSWIRE_HOG_WARN_MS 250
#endif

class ThreadsafeWire {
public:
    // `name` appears in every stall report, so a board with two wrapped buses
    // can tell them apart in the log: ThreadsafeWire(Wire1, 47, 48, "I2C1").
    // It is not copied — pass a string literal or something equally long-lived.
    ThreadsafeWire(TwoWire& wire, int sda, int scl, const char* name = "I2C");

    const char* name() const { return _name; }

    static constexpr uint32_t STALL_WARN_MS   = CHIPGUY_TSWIRE_STALL_WARN_MS;
    static constexpr uint32_t STALL_GIVEUP_MS = CHIPGUY_TSWIRE_STALL_GIVEUP_MS;
    static constexpr uint32_t HOG_WARN_MS     = CHIPGUY_TSWIRE_HOG_WARN_MS;

    // Returned by endTransmission() when the bus could not be acquired at all.
    // Deliberately 5, the same code the ESP32 core uses for a timeout, so
    // existing "nonzero means it failed" handling needs no changes.
    static constexpr uint8_t ERR_BUS_STALLED = 5;

    // Normal priority. You rarely need these — every transaction locks itself
    // (see above). Reach for them only to make SEVERAL transactions atomic as a
    // group. Registers demand for the bus before blocking, so a background
    // streamer knows to get out of the way; only the outermost acquisition by a
    // task counts, so nesting is free.
    //
    // Returns false if the bus could not be acquired within STALL_GIVEUP_MS, in
    // which case the mutex is NOT held and unlock() must not be called. Use the
    // Guard, which gets this right for you.
    [[nodiscard]] bool lock();
    void unlock();

    // Background priority, for bulk transfers. Takes the same mutex — mutual
    // exclusion is identical — but does not register demand, so a streamer
    // holding this does not mistake itself for a caller it should yield to.
    [[nodiscard]] bool lockBackground();
    void unlockBackground();

    // How many times acquisition has given up, and how many times a caller has
    // held the bus longer than HOG_WARN_MS while a streamer waited. Both should
    // be zero forever; either being nonzero means go read the serial log.
    uint32_t getBusStalls() const { return _busStalls.load(std::memory_order_relaxed); }
    uint32_t getBusHogs()   const { return _busHogs.load(std::memory_order_relaxed); }

    // True while at least one normal-priority caller is waiting for the bus or
    // holding it. Meaningful to a background streamer only between its chunks,
    // when it holds nothing of its own.
    bool isWanted() const { return _waiters.load(std::memory_order_relaxed) != 0; }

    // Called by a background streamer between chunks once isWanted() is true:
    // blocks until the normal-priority callers have taken the bus and released
    // it.
    //
    // The delay is what actually hands the bus over: a streaming task typically
    // runs at high priority, so simply releasing the mutex is not enough — it
    // would win the race to re-acquire it every time and starve the waiter.
    //
    // The bound is deliberately LONG. Anything holding this bus for more than a
    // few tens of milliseconds is sloppy code, and the most useful thing that
    // can happen is for the bulk transfer to visibly stall while it does — a
    // frozen screen sends someone to the serial log, which by then says exactly
    // who is holding the bus and for how long. Resuming early would instead have
    // the streamer fight the hog for the bus, hiding the symptom and slowing
    // both. The bound exists only so a caller that never releases degrades the
    // transfer rather than killing it permanently.
    void yieldToWaiters(uint32_t maxWaitMs = STALL_GIVEUP_MS);

    // Both guards release only if they actually acquired, so a stalled bus can
    // never produce a spurious unlock. Check acquired() (or the bool conversion)
    // before touching the bus:
    //
    //     ThreadsafeWire::Guard g(board.Wire);
    //     if (!g) return false;          // bus stalled; already reported loudly
    class Guard {
    public:
        explicit Guard(ThreadsafeWire& w) : _w(w), _ok(w.lock()) {}
        ~Guard() { if (_ok) _w.unlock(); }
        bool acquired() const { return _ok; }
        explicit operator bool() const { return _ok; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        ThreadsafeWire& _w;
        bool _ok;
    };

    class BackgroundGuard {
    public:
        explicit BackgroundGuard(ThreadsafeWire& w) : _w(w), _ok(w.lockBackground()) {}
        ~BackgroundGuard() { if (_ok) _w.unlockBackground(); }
        bool acquired() const { return _ok; }
        explicit operator bool() const { return _ok; }
        BackgroundGuard(const BackgroundGuard&) = delete;
        BackgroundGuard& operator=(const BackgroundGuard&) = delete;
    private:
        ThreadsafeWire& _w;
        bool _ok;
    };

    // Idempotent. The first call configures the bus on the wired SDA/SCL pins;
    // subsequent calls return true without touching hardware.
    bool begin();
    bool begin(int sda, int scl, uint32_t frequency = 0);
    bool end();

    bool setClock(uint32_t freq);
    uint32_t getClock();

    void setTimeOut(uint16_t ms);
    uint16_t getTimeOut();

    void beginTransmission(uint8_t address);
    void beginTransmission(int address);
    uint8_t endTransmission();
    uint8_t endTransmission(bool sendStop);

    size_t requestFrom(uint8_t address, size_t size);
    size_t requestFrom(uint8_t address, size_t size, bool sendStop);
    size_t requestFrom(int address, int size);
    size_t requestFrom(int address, int size, int sendStop);

    size_t write(uint8_t data);
    size_t write(const uint8_t* data, size_t len);

    int available();
    int read();
    int peek();
    void flush();

    void onReceive(void (*cb)(int));
    void onRequest(void (*cb)());

    // Convenience: register read/write for devices with an 8-bit register
    // index (the overwhelming majority of I2C peripherals). These do the
    // full repeated-start sequence under the mutex, so callers don't have
    // to manage a Guard. Return true on success.
    bool readRegister(uint8_t address, uint8_t reg, uint8_t* buf, size_t len);
    bool readRegister(uint8_t address, uint8_t reg, uint8_t& value);
    bool writeRegister(uint8_t address, uint8_t reg, const uint8_t* buf, size_t len);
    bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);

    // Convenience: single-byte transactions for register-less devices
    // (e.g. PCF8574, where a write sets all 8 output latches and a read
    // returns the current pin levels).
    bool writeByte(uint8_t address, uint8_t value);
    bool readByte(uint8_t address, uint8_t& value);

    // Address-only probe: START, address byte, STOP. True if the device ACKed.
    // This is what an I2C scanner does, and it is how a board class finds out
    // whether a chip is actually fitted — which is what makes a begin() that
    // tolerates missing hardware possible.
    bool probe(uint8_t address);

    TwoWire& raw() { return _wire; }

private:
    TwoWire& _wire;
    int _sda;
    int _scl;
    const char* _name;
    SemaphoreHandle_t _mutex;
    bool _began;

    // Recursion depth of the current holder. Only touched while holding the
    // mutex, so it needs no protection of its own. Used to register bus demand
    // exactly once per outermost acquisition.
    uint32_t _depth = 0;

    // Whether the outermost acquisition currently in force registered demand.
    // Recorded rather than re-derived, so release decrements _waiters exactly
    // when the matching acquire incremented it — even if a caller pairs
    // lockBackground() with unlock() or vice versa.
    bool _outermostCounted = false;

    // Which task, if any, has a transaction open (between beginTransmission()
    // and its completion). Only set by a task holding the mutex, only compared
    // against the current task.
    TaskHandle_t _txnOwner = nullptr;

    std::atomic<uint32_t> _busStalls{0};
    std::atomic<uint32_t> _busHogs{0};

    bool _openTxn();
    void _closeTxn();
    bool _txnMine() const;

    // Shared body of lock()/lockBackground(): stall silently, then report, then
    // give up. `background` selects whether demand is registered.
    bool _take(bool background);
    // Shared body of unlock()/unlockBackground().
    void _give();
    void _report(const char *what, uint32_t waitedMs) const;

    // Normal-priority callers waiting for or holding the bus. Incremented before
    // blocking so demand is visible to a background streamer while it still has
    // a chance to act on it, not only once the caller has won the mutex.
    std::atomic<uint32_t> _waiters{0};
};
