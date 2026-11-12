#include "chipguy_ThreadsafeWire.h"

ThreadsafeWire::ThreadsafeWire(TwoWire& wire, int sda, int scl, const char* name)
    : _wire(wire), _sda(sda), _scl(scl), _name(name ? name : "I2C"),
      _mutex(xSemaphoreCreateRecursiveMutex()), _began(false) {}

// Dump everything a person would want in order to find whoever wedged the bus.
// Deliberately verbose: by the time this prints, the device has already been
// visibly frozen for seconds, and the only thing that matters is that the log
// names the culprit.
void ThreadsafeWire::_report(const char *what, uint32_t waitedMs) const {
    TaskHandle_t self   = xTaskGetCurrentTaskHandle();
    TaskHandle_t holder = xSemaphoreGetMutexHolder(_mutex);

    Serial.printf("\n[%s] %s after %lu ms\n", _name, what, (unsigned long)waitedMs);
    Serial.printf("  waiting task : '%s' (priority %u, core %d)\n",
                  pcTaskGetName(self), (unsigned)uxTaskPriorityGet(self),
                  (int)xPortGetCoreID());
    if (holder) {
        Serial.printf("  bus held by  : '%s' (priority %u), nesting depth %u\n",
                      pcTaskGetName(holder), (unsigned)uxTaskPriorityGet(holder),
                      (unsigned)_depth);
    } else {
        Serial.printf("  bus held by  : nobody — the mutex is free, so this is "
                      "contention rather than a stuck holder\n");
    }
    Serial.printf("  open txn     : %s\n",
                  _txnOwner ? pcTaskGetName(_txnOwner)
                            : "none (no beginTransmission outstanding)");
    Serial.printf("  waiters %u, stalls %u, hogs %u\n",
                  (unsigned)_waiters.load(std::memory_order_relaxed),
                  (unsigned)_busStalls.load(std::memory_order_relaxed),
                  (unsigned)_busHogs.load(std::memory_order_relaxed));
    Serial.printf("  usual causes : beginTransmission() with no matching "
                  "endTransmission() (often an early return in between);\n"
                  "                 endTransmission(false) whose requestFrom() "
                  "never came;\n"
                  "                 a ThreadsafeWire::Guard held across "
                  "something slow (delay, network, Serial);\n"
                  "                 a task deleted while holding the bus.\n");
}

// Shared acquisition path. See the STALL section in the header for why the
// timings are shaped this way: silence, then noise, then a clean failure.
//
// Normal priority announces demand BEFORE blocking. A background streamer polls
// isWanted() between its chunks, and it can only act on demand it can see — if
// the counter only rose once the mutex had been won, the streamer would never
// learn that anyone was queued behind it.
//
// Only the OUTERMOST acquisition by a task counts. The mutex is recursive and
// the wrapper methods nest freely inside each other and inside caller Guards;
// counting every level would inflate demand and, worse, would make the streaming
// task read its own nested takes as somebody else wanting the bus.
bool ThreadsafeWire::_take(bool background) {
    const bool outermost =
        (xSemaphoreGetMutexHolder(_mutex) != xTaskGetCurrentTaskHandle());
    const bool counts = (!background && outermost);
    if (counts) _waiters.fetch_add(1, std::memory_order_relaxed);

    // Fast path, and the deliberate silent stall. Nothing is printed for the
    // first STALL_WARN_MS: a device that freezes for a few seconds is the bug
    // report, and logging instantly would just bury it in noise.
    if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(STALL_WARN_MS)) == pdTRUE) {
        // Safe: we hold the mutex now. Record whether this outermost hold is
        // the one that registered demand, so _give() can undo exactly it.
        if (_depth++ == 0) _outermostCounted = counts;
        return true;
    }

    uint32_t waited = STALL_WARN_MS;
    _report("BUS STALLED", waited);

    while (waited < STALL_GIVEUP_MS) {
        if (xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (_depth++ == 0) _outermostCounted = counts;
            Serial.printf("[%s] bus recovered after %lu ms; '%s' proceeding\n",
                          _name, (unsigned long)(waited + 1000),
                          pcTaskGetName(xTaskGetCurrentTaskHandle()));
            return true;
        }
        waited += 1000;
        _report("still stalled", waited);
    }

    _busStalls.fetch_add(1, std::memory_order_relaxed);
    _report("GIVING UP — this transaction will fail", waited);
    Serial.printf("  the bus is NOT being taken without the mutex: doing so "
                  "would corrupt\n  whatever the holder is mid-way through, and "
                  "would block on the platform's\n  own I2C lock regardless. "
                  "endTransmission() will return %u.\n",
                  (unsigned)ERR_BUS_STALLED);

    if (counts) _waiters.fetch_sub(1, std::memory_order_relaxed);
    return false;
}

// Release. _outermostCounted is read while the mutex is still held, and cleared
// with it, so demand is dropped exactly once per counted acquisition regardless
// of which unlock() variant the caller used.
void ThreadsafeWire::_give() {
    const bool outermost = (--_depth == 0);
    const bool counted   = outermost && _outermostCounted;
    if (outermost) _outermostCounted = false;
    xSemaphoreGiveRecursive(_mutex);
    if (counted) _waiters.fetch_sub(1, std::memory_order_relaxed);
}

bool ThreadsafeWire::lock()           { return _take(false); }
bool ThreadsafeWire::lockBackground() { return _take(true); }
void ThreadsafeWire::unlock()           { _give(); }
void ThreadsafeWire::unlockBackground() { _give(); }

void ThreadsafeWire::yieldToWaiters(uint32_t maxWaitMs) {
    const uint32_t start = millis();
    bool warned = false;

    while (isWanted()) {
        // taskYIELD() would not be enough: the streaming task typically runs at
        // a higher priority than the callers it is standing aside for, so it has
        // to leave the ready list for a tick to let them actually run.
        vTaskDelay(1);

        const uint32_t held = millis() - start;

        if (!warned && held >= HOG_WARN_MS) {
            warned = true;
            _busHogs.fetch_add(1, std::memory_order_relaxed);
            Serial.printf("\n[%s] a normal-priority caller has held the bus for "
                          "%lu ms.\n"
                          "  The background transfer is stalled and will stay "
                          "stalled until it lets go —\n"
                          "  that is deliberate, so the frozen screen leads you "
                          "here. Anything over ~%lu ms\n"
                          "  on this bus wants looking at.\n",
                          _name, (unsigned long)held, (unsigned long)HOG_WARN_MS);
            _report("bus hog detected", held);
        }

        if (held >= maxWaitMs) {
            Serial.printf("[%s] giving up standing aside after %lu ms; the "
                          "background transfer will\n  resume and now competes "
                          "with the holder for the bus.\n",
                          _name, (unsigned long)held);
            break;
        }
    }
}

bool ThreadsafeWire::begin() {
    Guard g(*this);
    if (!g) return false;
    if (_began) return true;
    _began = _wire.begin(_sda, _scl);
    return _began;
}

bool ThreadsafeWire::begin(int sda, int scl, uint32_t frequency) {
    Guard g(*this);
    if (!g) return false;
    if (_began) return true;
    _sda = sda;
    _scl = scl;
    _began = _wire.begin(sda, scl, frequency);
    return _began;
}

bool ThreadsafeWire::end() {
    Guard g(*this);
    if (!g) return false;
    if (!_began) return true;
    bool ok = _wire.end();
    _began = false;
    return ok;
}

// Every one of these checks the Guard before touching _wire. A stalled bus has
// already been reported at length by the time the Guard comes back false; what
// matters here is that nothing reaches the hardware, or the shared RX buffer,
// without the mutex. The failure values are the ones each call's existing error
// handling already understands: false, 0, or -1 for "no byte".
bool ThreadsafeWire::setClock(uint32_t freq) { Guard g(*this); return g ? _wire.setClock(freq) : false; }
uint32_t ThreadsafeWire::getClock()          { Guard g(*this); return g ? _wire.getClock() : 0; }

void ThreadsafeWire::setTimeOut(uint16_t ms) { Guard g(*this); if (g) _wire.setTimeOut(ms); }
uint16_t ThreadsafeWire::getTimeOut()        { Guard g(*this); return g ? _wire.getTimeOut() : 0; }

// ── Transaction-scoped locking ─────────────────────────────────────────────
//
// beginTransmission() takes the mutex and HOLDS it until the transaction
// completes — endTransmission(true), or requestFrom() when a repeated start is
// outstanding. That makes a plain
//
//     wire.beginTransmission(addr); wire.write(x); wire.endTransmission();
//
// atomic with no lock() call anywhere in sight, which is the point: code ported
// from TwoWire by changing the type is correct as written.
//
// Guarding each method individually instead would be worse than useless. The
// ESP32's own TwoWire takes an internal HAL semaphore in beginTransmission() and
// releases it in endTransmission(true) — so a task that grabbed our mutex
// between another task's begin and end would block on that HAL lock while
// holding ours, and the first task would block on ours while holding the HAL
// lock. A textbook AB-BA deadlock. Acquiring our mutex first and holding it for
// the whole transaction makes the two locks always nest in the same order.
//
// It also means demand registration spans the whole transaction rather than
// flickering on and off per call, so the streaming task reliably sees it.
//
// _txnOwner records which task has a transaction open. It is only ever set by a
// task holding the mutex, and only ever compared against the current task, so a
// task that does not hold the mutex can never mistake someone else's open
// transaction for its own.

bool ThreadsafeWire::_openTxn() {
    if (!lock()) return false;      // stalled; already reported at length
    _txnOwner = xTaskGetCurrentTaskHandle();
    return true;
}

void ThreadsafeWire::_closeTxn() {
    _txnOwner = nullptr;
    unlock();
}

bool ThreadsafeWire::_txnMine() const {
    return _txnOwner == xTaskGetCurrentTaskHandle();
}

void ThreadsafeWire::beginTransmission(uint8_t address) {
    // If the bus could not be acquired, deliberately do NOT call through to the
    // hardware. _txnOwner stays unset, which is how endTransmission() below
    // knows to report the failure instead of ending a transaction that was never
    // begun. Touching TwoWire here without the mutex would corrupt whatever the
    // holder is doing, and would block on the platform's own I2C lock anyway.
    if (!_openTxn()) return;
    _wire.beginTransmission(address);
}

void ThreadsafeWire::beginTransmission(int address) {
    beginTransmission((uint8_t)address);
}

uint8_t ThreadsafeWire::endTransmission() { return endTransmission(true); }

uint8_t ThreadsafeWire::endTransmission(bool sendStop) {
    // No open transaction of ours means beginTransmission() failed to get the
    // bus. Report it the way every I2C caller already handles — a nonzero
    // status — rather than driving hardware we do not own.
    if (!_txnMine()) return ERR_BUS_STALLED;

    const uint8_t r = _wire.endTransmission(sendStop);
    // sendStop == false is a repeated start: the underlying Wire keeps its HAL
    // lock for the requestFrom() still to come, so we keep ours too. Mirroring
    // the platform's transaction lifetime exactly is what keeps the two locks in
    // step. (A caller that opens a repeated start and never completes it leaks a
    // level here — and leaks the platform's HAL lock identically, so this is no
    // worse than calling TwoWire directly.)
    if (sendStop) _closeTxn();
    return r;
}

size_t ThreadsafeWire::requestFrom(uint8_t address, size_t size, bool sendStop) {
    if (_txnMine()) {
        // Completing a repeated start begun by beginTransmission(): we already
        // hold the mutex, and this call ends the transaction.
        const size_t n = _wire.requestFrom(address, size, sendStop);
        _closeTxn();
        return n;
    }
    // Standalone read: self-contained, so lock just for its duration.
    Guard g(*this);
    if (!g) return 0;               // bus stalled; already reported at length
    return _wire.requestFrom(address, size, sendStop);
}

size_t ThreadsafeWire::requestFrom(uint8_t address, size_t size) {
    return requestFrom(address, size, true);
}
size_t ThreadsafeWire::requestFrom(int address, int size) {
    return requestFrom((uint8_t)address, (size_t)size, true);
}
size_t ThreadsafeWire::requestFrom(int address, int size, int sendStop) {
    return requestFrom((uint8_t)address, (size_t)size, sendStop != 0);
}

// A short write is how a caller learns the byte did not make it, so a stalled
// bus reports zero rather than pretending to have buffered anything.
size_t ThreadsafeWire::write(uint8_t data)                    { Guard g(*this); return g ? _wire.write(data) : 0; }
size_t ThreadsafeWire::write(const uint8_t* data, size_t len) { Guard g(*this); return g ? _wire.write(data, len) : 0; }

int  ThreadsafeWire::available() { Guard g(*this); return g ? _wire.available() : 0; }
int  ThreadsafeWire::read()      { Guard g(*this); return g ? _wire.read() : -1; }
int  ThreadsafeWire::peek()      { Guard g(*this); return g ? _wire.peek() : -1; }
void ThreadsafeWire::flush()     { Guard g(*this); if (g) _wire.flush(); }

void ThreadsafeWire::onReceive(void (*cb)(int)) { Guard g(*this); if (g) _wire.onReceive(cb); }
void ThreadsafeWire::onRequest(void (*cb)())    { Guard g(*this); if (g) _wire.onRequest(cb); }

bool ThreadsafeWire::readRegister(uint8_t address, uint8_t reg, uint8_t* buf, size_t len) {
    Guard g(*this);
    if (!g) return false;           // bus stalled; already reported at length
    _wire.beginTransmission(address);
    _wire.write(reg);
    if (_wire.endTransmission(false) != 0) return false;
    if (_wire.requestFrom(address, len) != len) return false;
    for (size_t i = 0; i < len; ++i) {
        int b = _wire.read();
        if (b < 0) return false;
        buf[i] = (uint8_t)b;
    }
    return true;
}

bool ThreadsafeWire::readRegister(uint8_t address, uint8_t reg, uint8_t& value) {
    return readRegister(address, reg, &value, 1);
}

bool ThreadsafeWire::writeRegister(uint8_t address, uint8_t reg, const uint8_t* buf, size_t len) {
    Guard g(*this);
    if (!g) return false;           // bus stalled; already reported at length
    _wire.beginTransmission(address);
    _wire.write(reg);
    if (len > 0) _wire.write(buf, len);
    return _wire.endTransmission() == 0;
}

bool ThreadsafeWire::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
    return writeRegister(address, reg, &value, 1);
}

bool ThreadsafeWire::writeByte(uint8_t address, uint8_t value) {
    Guard g(*this);
    if (!g) return false;           // bus stalled; already reported at length
    _wire.beginTransmission(address);
    _wire.write(value);
    return _wire.endTransmission() == 0;
}

bool ThreadsafeWire::readByte(uint8_t address, uint8_t& value) {
    Guard g(*this);
    if (!g) return false;           // bus stalled; already reported at length
    if (_wire.requestFrom(address, (size_t)1) != 1) return false;
    int b = _wire.read();
    if (b < 0) return false;
    value = (uint8_t)b;
    return true;
}

bool ThreadsafeWire::probe(uint8_t address) {
    Guard g(*this);
    if (!g) return false;           // bus stalled; already reported at length
    // Zero payload: the core emits START, the address byte, then STOP, and
    // reports 0 only if the device pulled SDA low to acknowledge.
    _wire.beginTransmission(address);
    return _wire.endTransmission() == 0;
}
