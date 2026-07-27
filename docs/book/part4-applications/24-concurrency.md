---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 24: Concurrency

You tap an app on your phone. You expect it to respond instantly. But what if the app is downloading a file? Or saving to the cloud? Or processing a photo?

If a program can only do one thing at a time, everything waits. You tap a button, and the app freezes while it fetches data from a server. You resize an image, and the entire interface locks up. The screen goes white. The dreaded "Not Responding" appears.

This is not acceptable. Modern users expect applications that stay responsive, that handle multiple tasks smoothly, that take advantage of all those CPU cores sitting inside their devices.

This chapter teaches you to write programs that do many things at once. This is *concurrency* — one of the most powerful and challenging topics in programming.

---

## Why Concurrency Matters

Consider a program that downloads 100 images:

```zia
// Sequential: ~100 seconds (1 second per image)
bind Http = Zanna.Network.Http;

for url in urls {
    var image = Http.Get(url);
    images.Push(image);
}
```

Most of that time is spent *waiting*. Your CPU, capable of billions of operations per second, sits idle while the network responds. Now imagine running downloads in parallel — same work, 50-100x faster.

Concurrency lets your programs:

- **Stay responsive**: The user interface remains active while background work proceeds
- **Run faster**: Multiple CPU cores work on different parts of a problem simultaneously
- **Model reality**: The real world is concurrent — multiple things happen at once
- **Handle many clients**: A web server must handle thousands of users at the same time

But concurrency comes with serious challenges. When multiple things happen simultaneously, they can interfere with each other in subtle, hard-to-reproduce ways. This chapter teaches both the power and the pitfalls.

---

## 24.1 Threads

A *thread* is an independent path of execution within your program. All threads share the same memory, but each has its own instruction pointer — its own "place" in the code.

Zanna provides threads through the `Zanna.Threads.Thread` class.

### Starting a Thread

```zia
bind Thread = Zanna.Threads.Thread;
bind Zanna.Terminal as Terminal;

func helloWorker(arg: Any) {
    Terminal.Say("Hello from another thread!");
}

func start() {
    // Start a new thread that runs a function
    var t = Thread.Start(&helloWorker, 0);

    // Wait for the thread to finish
    t.Join();
    Terminal.Say("Thread completed.");
}
```

`Thread.Start` takes a function reference plus one argument and runs it on a new OS thread. It returns a thread handle that you use to manage the thread's lifecycle.

### Thread Properties

Every thread handle exposes useful properties:

| Property | Type | Description |
|----------|------|-------------|
| `Id` | `Integer` | OS-assigned thread identifier |
| `IsAlive` | `Boolean` | Whether the thread is still running |
| `HasError` | `Boolean` | Whether the thread terminated with an error |
| `Error` | `String` | Error message (if `HasError` is true) |

```zia
bind Thread = Zanna.Threads.Thread;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

func sleepyWorker(arg: Any) {
    Thread.Sleep(100);  // Sleep 100 milliseconds
}

func start() {
    var t = Thread.StartSafe(&sleepyWorker, 0);

    Terminal.Say("Thread ID: " + Fmt.Int(t.SafeGetId()));
    Terminal.Say("Alive? " + Fmt.Bool(t.SafeIsAlive()));

    t.SafeJoin();
    Terminal.Say("Still alive? " + Fmt.Bool(t.SafeIsAlive()));
}
```

### Joining Threads

`Join()` blocks the calling thread until the target thread finishes. Without joining, the main thread might exit before worker threads complete.

```zia
bind Thread = Zanna.Threads.Thread;

func slowWorker(arg: Any) {
    // Simulate work
    Thread.Sleep(500);
}

func start() {
    var t = Thread.Start(&slowWorker, 0);

    // TryJoin returns immediately: true if done, false if still running
    var done = t.TryJoin();

    // JoinFor waits up to N milliseconds: true if done, false if timed out
    var finished = t.JoinFor(1000);
}
```

### Safe Thread Operations

`StartSafe` wraps the thread body in error handling, so unhandled errors don't crash the program:

```zia
bind Thread = Zanna.Threads.Thread;
bind Box = Zanna.Core.Box;
bind Zanna.Terminal as Terminal;

func riskyWorker(arg: Any) {
    // If this traps, the error is captured by StartSafe
    var divisor = Box.ToI64(arg);
    var x = 1 / divisor;
}

func start() {
    var t = Thread.StartSafeOwned(&riskyWorker, Box.I64(0));

    t.SafeJoin();  // Safe join that won't propagate errors

    if t.HasError {
        Terminal.Say("Thread error: " + t.Error);
    }
}
```

### Thread Utilities

| Method | Description |
|--------|-------------|
| `Thread.Sleep(ms)` | Pause the current thread for `ms` milliseconds |
| `Thread.Yield()` | Give other threads a chance to run |

---

## 24.2 Shared State and Monitors

When threads share data, they can corrupt it. Two threads incrementing a counter simultaneously might both read the same value, both add 1, and both write back — losing an increment. This is a *race condition*.

### Monitor (Mutual Exclusion)

A `Monitor` provides mutual exclusion: only one thread can hold the monitor at a time. Other threads that try to enter will block until it's released.

```zia
bind Thread = Zanna.Threads.Thread;
bind Monitor = Zanna.Threads.Monitor;
bind Map = Zanna.Collections.Map;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var counter = 0;
var lock = Map.New();  // Any runtime object can serve as a monitor target

func incrementCounter() {
    Monitor.Enter(lock);
    counter = counter + 1;
    Monitor.Exit(lock);
}

func incrementWorker(arg: Any) {
    for j in 0..500 {
        incrementCounter();
    }
}

func start() {
    var t1 = Thread.Start(&incrementWorker, 0);
    var t2 = Thread.Start(&incrementWorker, 0);

    t1.Join();
    t2.Join();

    Terminal.Say("Counter: " + Fmt.Int(counter));  // Should be 1000
}
```

### Monitor Methods

| Method | Description |
|--------|-------------|
| `Monitor.Enter(obj)` | Acquire the monitor (blocks if held) |
| `Monitor.TryEnter(obj)` | Try to acquire without blocking (returns `Boolean`) |
| `Monitor.TryEnterFor(obj, ms)` | Try to acquire with timeout |
| `Monitor.Exit(obj)` | Release the monitor |
| `Monitor.Wait(obj)` | Release and wait for notification |
| `Monitor.WaitFor(obj, ms)` | Wait with timeout |
| `Monitor.Notify(obj)` | Notify one waiting thread |
| `Monitor.NotifyAll(obj)` | Notify all waiting threads |

### Producer-Consumer with Monitor

```zia
bind Thread = Zanna.Threads.Thread;
bind Monitor = Zanna.Threads.Monitor;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var buffer: List[Integer] = [];
var lock = Zanna.Collections.Map.New();
var done = false;

func producer() {
    for i in 1..=10 {
        Monitor.Enter(lock);
        buffer.Push(i);
        Monitor.Notify(lock);    // Wake a waiting consumer
        Monitor.Exit(lock);
        Thread.Sleep(50);
    }

    Monitor.Enter(lock);
    done = true;
    Monitor.NotifyAll(lock);     // Wake all consumers
    Monitor.Exit(lock);
}

func consumer(id: Integer) {
    while true {
        Monitor.Enter(lock);
        while (buffer.count() == 0 && !done) {
            Monitor.Wait(lock);  // Release lock and sleep until notified
        }
        if (buffer.count() > 0) {
            var item = buffer.get(0);
            buffer.removeAt(0);
            Monitor.Exit(lock);
            Terminal.Say("Consumer " + Fmt.Int(id) + " got: " + Fmt.Int(item));
        } else {
            Monitor.Exit(lock);
            if (done) { break; }
        }
    }
}
```

### SafeI64 (Atomic Integer)

For simple numeric shared state, `SafeI64` provides lock-free atomic operations:

```zia
bind Thread = Zanna.Threads.Thread;
bind SafeI64 = Zanna.Threads.SafeI64;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var counter = SafeI64.New(0);

func addWorker(arg: Any) {
    for i in 0..1000 {
        counter.Add(1);  // Atomic increment
    }
}

func start() {
    counter.Set(0);

    var t1 = Thread.Start(&addWorker, 0);
    var t2 = Thread.Start(&addWorker, 0);

    t1.Join();
    t2.Join();

    Terminal.Say("Counter: " + Fmt.Int(counter.Get()));  // Always 2000
}
```

| Method | Description |
|--------|-------------|
| `SafeI64.New(initial)` | Create with initial value |
| `.Get()` | Read current value atomically |
| `.Set(value)` | Write value atomically |
| `.Add(delta)` | Add and return new value atomically |
| `.CompareExchange(expected, desired)` | CAS: set to `desired` if current equals `expected`, returns old value |

The `CompareExchange` operation (CAS) is the building block for lock-free algorithms:

```zia
bind SafeI64 = Zanna.Threads.SafeI64;

// Lock-free maximum update
func atomicMax(atom: SafeI64, candidate: Integer) {
    while true {
        var current = atom.Get();
        if (candidate <= current) { break; }
        var old = atom.CompareExchange(current, candidate);
        if (old == current) { break; }  // Success
        // Another thread changed it — retry
    }
}
```

---

## 24.3 Synchronization Primitives

Beyond monitors, Zanna provides specialized synchronization tools for different coordination patterns.

### Gate (Semaphore)

A `Gate` controls access to a limited resource. It maintains a count of available permits. Threads enter (consuming a permit) and leave (releasing a permit).

```zia
bind Thread = Zanna.Threads.Thread;
bind Gate = Zanna.Threads.Gate;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var dbGate = Gate.New(3);

func queryWorker(arg: Any) {
    var id = Box.ToI64(arg);
    dbGate.Enter();              // Wait for a permit
    Terminal.Say("Query " + Fmt.Int(id) + " running");
    Thread.Sleep(200);           // Simulate query
    dbGate.Leave();              // Release permit
}

func start() {
    var t1 = Thread.StartOwned(&queryWorker, Box.I64(1));
    var t2 = Thread.StartOwned(&queryWorker, Box.I64(2));
    var t3 = Thread.StartOwned(&queryWorker, Box.I64(3));

    t1.Join();
    t2.Join();
    t3.Join();
}
```

| Method | Description |
|--------|-------------|
| `Gate.New(permits)` | Create with N initial permits |
| `.Enter()` | Acquire one permit (blocks if none available) |
| `.TryEnter()` | Try without blocking |
| `.TryEnterFor(ms)` | Try with timeout |
| `.Leave()` | Release one permit |
| `.Leave(n)` | Release N permits |
| `.Permits` | Current available permit count |

`Gate.Leave` and `Gate.Leave(n)` trap if the permit count would overflow instead of wrapping.

### Barrier

A `Barrier` makes multiple threads wait until all of them have arrived at a synchronization point before any can proceed. Useful for phased algorithms.

```zia
bind Thread = Zanna.Threads.Thread;
bind Barrier = Zanna.Threads.Barrier;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var barrier = Barrier.New(3);  // Wait for 3 threads

func phaseWorker(arg: Any) {
    var id = Box.ToI64(arg);
    Terminal.Say("Thread " + Fmt.Int(id) + " phase 1 done");
    barrier.Arrive();  // Wait until all 3 threads arrive
    Terminal.Say("Thread " + Fmt.Int(id) + " phase 2 starting");
}

func start() {
    var t1 = Thread.StartOwned(&phaseWorker, Box.I64(1));
    var t2 = Thread.StartOwned(&phaseWorker, Box.I64(2));
    var t3 = Thread.StartOwned(&phaseWorker, Box.I64(3));

    t1.Join();
    t2.Join();
    t3.Join();
}
```

| Method/Property | Description |
|-----------------|-------------|
| `Barrier.New(parties)` | Create for N parties |
| `.Arrive()` | Arrive and wait; returns arrival index |
| `.Reset()` | Reset for reuse |
| `.Parties` | Total parties expected |
| `.Waiting` | Number currently waiting |

### RwLock (Reader-Writer Lock)

A `RwLock` allows multiple simultaneous readers *or* one exclusive writer. This is more efficient than a Monitor when reads vastly outnumber writes.

```zia
bind Thread = Zanna.Threads.Thread;
bind RwLock = Zanna.Threads.RwLock;

var cache: List[String] = [];
var rwLock = RwLock.New();

func readCache(index: Integer) -> String {
    rwLock.ReadEnter();
    var result = cache.get(index);
    rwLock.ReadExit();
    return result;
}

func writeCache(value: String) {
    rwLock.WriteEnter();
    cache.Push(value);
    rwLock.WriteExit();
}
```

| Method/Property | Description |
|-----------------|-------------|
| `RwLock.New()` | Create a new reader-writer lock |
| `.ReadEnter()` / `.ReadExit()` | Acquire/release read access |
| `.WriteEnter()` / `.WriteExit()` | Acquire/release write access |
| `.TryReadEnter()` | Try read lock without blocking |
| `.TryWriteEnter()` | Try write lock without blocking |
| `.Readers` | Number of current readers |
| `.IsWriteLocked` | Whether a writer holds the lock |

Read-to-write upgrades are rejected to avoid self-deadlock. A thread that already owns the write lock may enter and exit the read side, which supports explicit downgrade patterns when calls are balanced.

---

## 24.4 Channels

Channels provide safe communication between threads without shared mutable state. One thread *sends* data into the channel; another thread *receives* it. This is the "communicate by sharing" approach to concurrency.

```zia
bind Thread = Zanna.Threads.Thread;
bind Channel = Zanna.Threads.Channel;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var ch = Channel.New(5);  // Buffered channel with capacity 5

func producerWorker(arg: Any) {
    for i in 1..=10 {
        ch.Send(Box.I64(i));
        Terminal.Say("Sent: " + Fmt.Int(i));
    }
    ch.Close();
}

func consumerWorker(arg: Any) {
    while (!ch.IsClosed || !ch.IsEmpty) {
        var item = ch.Recv();
        if item != null {
            Terminal.Say("Received: " + Fmt.Int(Box.ToI64(item)));
        }
    }
}

func start() {
    var producer = Thread.Start(&producerWorker, 0);
    var consumer = Thread.Start(&consumerWorker, 0);

    producer.Join();
    consumer.Join();
}
```

### Channel Methods

| Method | Description |
|--------|-------------|
| `Channel.New(capacity)` | Create a buffered channel |
| `.Send(item)` | Send an item (blocks if full) |
| `.TrySend(item)` | Try to send without blocking |
| `.SendFor(item, ms)` | Send with timeout |
| `.Recv()` | Receive an item (blocks if empty) |
| `.TryRecv()` | Try to receive without blocking; returns `null` if empty |
| `.RecvFor(ms)` | Receive with timeout; returns `null` on timeout |
| `.Close()` | Close the channel (no more sends) |

At the C ABI layer, `rt_channel_try_recv(channel, NULL)` checks availability without consuming or releasing a value.

### Channel Properties

| Property | Type | Description |
|----------|------|-------------|
| `Length` | `Integer` | Number of items currently buffered |
| `Cap` | `Integer` | Maximum capacity |
| `IsClosed` | `Boolean` | Whether the channel has been closed |
| `IsEmpty` | `Boolean` | Whether the buffer is empty |
| `IsFull` | `Boolean` | Whether the buffer is at capacity |

### Pipeline Pattern

Channels naturally compose into pipelines where each stage processes data and passes results to the next:

```zia
bind Thread = Zanna.Threads.Thread;
bind Channel = Zanna.Threads.Channel;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var raw = Channel.New(10);
var processed = Channel.New(10);

func generateWorker(arg: Any) {
    for i in 1..=5 {
        raw.Send(Box.I64(i));
    }
    raw.Close();
}

func processWorker(arg: Any) {
    while (!raw.IsClosed || !raw.IsEmpty) {
        var item = raw.Recv();
        if item != null {
            var value = Box.ToI64(item);
            processed.Send(Box.I64(value * 2));
        }
    }
    processed.Close();
}

func start() {
    var generator = Thread.Start(&generateWorker, 0);
    var processor = Thread.Start(&processWorker, 0);

    // Stage 3: Consume results
    while (!processed.IsClosed || !processed.IsEmpty) {
        var result = processed.Recv();
        if result != null {
            Terminal.Say("Result: " + Fmt.Int(Box.ToI64(result)));
        }
    }

    generator.Join();
    processor.Join();
}
```

---

## 24.5 Thread Pools

Creating a new OS thread for every task is expensive. A *thread pool* maintains a set of reusable worker threads. You submit tasks; the pool assigns them to available workers.

```zia
bind Pool = Zanna.Threads.Pool;
bind Thread = Zanna.Threads.Thread;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

func poolTask(arg: Any) {
    var id = Box.ToI64(arg);
    Terminal.Say("Task " + Fmt.Int(id) + " running");
    Thread.Sleep(100);
}

func start() {
    var pool = Pool.New(4);  // 4 worker threads

    // Submit 20 tasks to the pool
    for i in 0..20 {
        Pool.Submit(pool, &poolTask, Box.I64(i));
    }

    pool.Wait();      // Wait for all tasks to complete
    pool.Shutdown();   // Clean up pool resources

    Terminal.Say("All tasks done.");
    Terminal.Say("Pool processed " + Fmt.Int(pool.Size) + " workers.");
}
```

### Pool Methods

| Method | Description |
|--------|-------------|
| `Pool.New(size)` | Create pool with N worker threads |
| `Pool.Submit(pool, callback, arg)` | Submit a task for execution |
| `.Wait()` | Block until all submitted tasks complete |
| `.WaitFor(ms)` | Wait with timeout |
| `.Shutdown()` | Graceful shutdown (finishes pending tasks) |
| `.ShutdownNow()` | Immediate shutdown (drops pending tasks) |

### Pool Properties

| Property | Type | Description |
|----------|------|-------------|
| `Size` | `Integer` | Number of worker threads |
| `Pending` | `Integer` | Tasks waiting to execute |
| `Active` | `Integer` | Tasks currently executing |
| `IsShutdown` | `Boolean` | Whether shutdown was requested |

Pool waits remain correct when a task traps: the worker still marks the task complete, so `Wait` and `WaitFor` do not hang. After the pool drains, `Wait`, successful `WaitFor`, `Shutdown`, and `ShutdownNow` surface the captured task trap instead of silently reporting success. Calls that would wait for or shut down the same pool from inside one of its workers trap to prevent self-deadlock.

---

## 24.6 Promises and Futures

A `Promise` represents a value that will be provided later. A `Future` is the read-side of a promise — it lets another thread wait for and retrieve the result.

```zia
bind Thread = Zanna.Threads.Thread;
bind Promise = Zanna.Threads.Promise;
bind Future = Zanna.Threads.Future;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

func computeWorker(p: Promise) {
    Thread.Sleep(200);  // Simulate computation
    p.Set(Box.I64(42));  // Fulfill the promise
}

func start() {
    var p = Promise.New();
    var f = p.GetFuture();

    // Worker thread computes a result
    var t = Thread.StartOwned(&computeWorker, p);

    // Main thread waits for the result
    var result = Box.ToI64(Future.Get(f));  // Blocks until the promise is fulfilled
    Terminal.Say("Got: " + Fmt.Int(result));  // "Got: 42"
    t.Join();
}
```

### Error Propagation

Promises can propagate errors to futures:

```zia
bind Thread = Zanna.Threads.Thread;
bind Promise = Zanna.Threads.Promise;
bind Future = Zanna.Threads.Future;
bind Zanna.Terminal as Terminal;

func failingPromiseWorker(p: Promise) {
    p.SetError("computation failed");
}

func start() {
    var p = Promise.New();
    var f = p.GetFuture();

    var t = Thread.StartOwned(&failingPromiseWorker, p);

    Future.Wait(f);
    if Future.get_IsError(f) {
        Terminal.Say("Error: " + Future.get_Error(f));
    }
    t.Join();
}
```

### Future Methods and Properties

| Method | Description |
|--------|-------------|
| `.Get()` | Block and retrieve the value |
| `.TryGet()` | Non-blocking get (returns null if not ready) |
| `.GetFor(ms)` | Get with timeout |
| `.Wait()` | Block until resolved (value or error) |
| `.WaitFor(ms)` | Wait with timeout |

| Property | Type | Description |
|----------|------|-------------|
| `IsDone` | `Boolean` | Whether the promise has been fulfilled |
| `IsError` | `Boolean` | Whether it resolved with an error |
| `Error` | `String` | Error message (if `IsError`) |

### Async Combinators

The `Async` class provides higher-level operations built on futures:

```zia
bind Async = Zanna.Threads.Async;
bind Box = Zanna.Core.Box;
bind Seq = Zanna.Collections.Seq;

func computeExpensiveValue(arg: Any) -> Any {
    return Box.I64(42);
}

func start() {
    // Run a function asynchronously, get a Future back
    var f = Async.Run(&computeExpensiveValue, 0);

    // Create a delayed future (resolves after N ms)
    var delayed = Async.Delay(1000);

    var futures = Seq.New();
    futures.Push(f);
    futures.Push(delayed);

    // Wait for ALL futures in a list
    var results = Async.All(futures);

    // Wait for ANY future (first to complete)
    var first = Async.Any(futures);
}
```

| Method | Description |
|--------|-------------|
| `Async.Run(func)` | Run asynchronously, returns a `Future` |
| `Async.RunCancellable(func, token)` | Run with cancellation support |
| `Async.Delay(ms)` | Future that resolves after N milliseconds |
| `Async.All(futures)` | Future that resolves when all complete |
| `Async.Any(futures)` | Future that resolves when any completes |
| `Async.Map(future, func)` | Transform a future's value |

Traps raised inside `Async.Run`, `Async.RunCancellable`, and `Async.Map` callbacks become Future errors. Callback-created runtime results are owned by the returned Future, so they remain valid for consumers after the worker exits; exact borrowed argument/input passthrough stays borrowed.

---

## 24.7 Concurrent Collections

Regular collections (`List`, `Map`) are **not** thread-safe. Accessing them from multiple threads without locks leads to corruption. Zanna provides two thread-safe collections.

### ConcurrentQueue

A thread-safe FIFO queue, ideal for producer-consumer patterns:

```zia
bind Thread = Zanna.Threads.Thread;
bind ConcurrentQueue = Zanna.Threads.ConcurrentQueue;
bind Box = Zanna.Core.Box;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var queue = ConcurrentQueue.New();

func queueProducer(arg: Any) {
    for i in 1..=10 {
        queue.Push(Box.I64(i));
        Thread.Sleep(50);
    }
}

func queueConsumer(arg: Any) {
    for i in 0..10 {
        var item = queue.Pop();  // Blocks if empty
        Terminal.Say("Got: " + Fmt.Int(Box.ToI64(item)));
    }
}

func start() {
    var producer = Thread.Start(&queueProducer, 0);
    var consumer = Thread.Start(&queueConsumer, 0);

    producer.Join();
    consumer.Join();
}
```

| Method | Description |
|--------|-------------|
| `ConcurrentQueue.New()` | Create an empty queue |
| `.Push(item)` | Add to the back |
| `.Pop()` | Remove from front (blocks if empty) |
| `.TryDequeue()` | Non-blocking dequeue (returns null if empty) |
| `.DequeueTimeout(ms)` | Dequeue with timeout |
| `.Peek()` | Look at front without removing |
| `.Clear()` | Remove all items |
| `.Length` | Number of items |
| `.IsEmpty` | Whether empty |

### ConcurrentMap

A thread-safe key-value store with string keys:

```zia
bind Thread = Zanna.Threads.Thread;
bind ConcurrentMap = Zanna.Threads.ConcurrentMap;
bind Box = Zanna.Core.Box;
bind Zanna.Terminal as Terminal;

var config = ConcurrentMap.New();

func configWriter(arg: Any) {
    config.Set("host", Box.Str("localhost"));
    config.Set("port", Box.Str("8080"));
}

func configReader(arg: Any) {
    var host = Box.ToStr(config.GetOr("host", Box.Str("127.0.0.1")));
    Terminal.Say("Connecting to " + host);
}

func start() {
    var writer = Thread.Start(&configWriter, 0);
    var reader = Thread.Start(&configReader, 0);

    writer.Join();
    reader.Join();
}
```

| Method | Description |
|--------|-------------|
| `ConcurrentMap.New()` | Create an empty map |
| `.Set(key, value)` | Insert or update |
| `.Get(key)` | Retrieve value (null if missing) |
| `.GetOr(key, default)` | Retrieve with default fallback |
| `.Has(key)` | Check if key exists |
| `.SetIfMissing(key, value)` | Set only if key doesn't exist (returns true if set) |
| `.Remove(key)` | Remove a key (returns true if found) |
| `.Clear()` | Remove all entries |
| `.Keys()` | Get all keys |
| `.Values()` | Get all values |
| `.Length` | Number of entries |
| `.IsEmpty` | Whether empty |

---

## 24.8 Parallel Utilities

The `Parallel` class provides high-level utilities for common parallel patterns without manual thread management:

```zia
bind Parallel = Zanna.Threads.Parallel;
bind Box = Zanna.Core.Box;
bind Seq = Zanna.Collections.Seq;
bind Zanna.Terminal as Terminal;

func processIndex(i: Integer) {
    // Process item i
}

func processItem(item: Any) {
    Terminal.Say("Processing: " + Box.ToStr(item));
}

func addBang(item: Any) -> Any {
    return Box.Str(Box.ToStr(item) + "!");
}

func start() {
    // Parallel for loop (partitioned across cores)
    Parallel.For(0, 1000, &processIndex);

    // Parallel for-each over a collection
    var items = Seq.New();
    items.Push(Box.Str("a"));
    items.Push(Box.Str("b"));
    items.Push(Box.Str("c"));
    items.Push(Box.Str("d"));
    Parallel.ForEach(items, &processItem);

    // Parallel map: transform each element concurrently
    var results = Parallel.Map(items, &addBang);
}
```

### Using a Custom Pool

By default, `Parallel` uses a shared default pool. You can provide your own:

```zia
bind Parallel = Zanna.Threads.Parallel;
bind Pool = Zanna.Threads.Pool;

func processIndex(i: Integer) {
    // Runs on the custom pool
}

func start() {
    var pool = Pool.New(2);  // Only 2 workers

    Parallel.ForPool(0, 100, &processIndex, pool);

    pool.Shutdown();
}
```

### Parallel Methods

| Method | Description |
|--------|-------------|
| `Parallel.For(start, end, func)` | Parallel for-loop over range |
| `Parallel.ForPool(start, end, func, pool)` | Same, with custom pool |
| `Parallel.ForEach(collection, func)` | Apply function to each item |
| `Parallel.ForEachPool(collection, func, pool)` | Same, with custom pool |
| `Parallel.Map(collection, func)` | Transform each item in parallel |
| `Parallel.MapPool(collection, func, pool)` | Same, with custom pool |
| `Parallel.Invoke(funcs)` | Run multiple functions in parallel |
| `Parallel.InvokePool(funcs, pool)` | Same, with custom pool |
| `Parallel.Reduce(collection, func, initial)` | Parallel reduction |
| `Parallel.ReducePool(collection, func, initial, pool)` | Same, with custom pool |
| `Parallel.DefaultWorkers()` | Number of default worker threads |
| `Parallel.DefaultPool()` | Access the shared default pool |

Parallel operations wake the caller and trap with a `Parallel.*: task trapped` message if a worker callback traps. `Parallel.Map` transfers mapper return values into the returned sequence; exact input passthrough is retained by the runtime, while other borrowed runtime objects must be retained by the mapper before return. `Parallel.Reduce` leaves accumulator ownership to the reducer and returns the final accumulator pointer as produced. Use named function references such as `&processItem`; Zia does not currently support inline function literals in these calls.

---

## 24.9 Cancellation

Long-running tasks should be cancellable. The `CancelToken` provides cooperative cancellation — a way for one part of the program to signal "stop" and for the running task to check periodically and comply.

```zia
bind Thread = Zanna.Threads.Thread;
bind CancelToken = Zanna.Threads.CancelToken;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

var token = CancelToken.New();

func cancellableWorker(arg: Any) {
    for i in 0..1000000 {
        // Periodically check for cancellation
        if token.IsCancelled {
            Terminal.Say("Cancelled at iteration " + Fmt.Int(i));
            return;
        }
        // Do work...
    }
}

func start() {
    var worker = Thread.Start(&cancellableWorker, 0);

    Thread.Sleep(100);   // Let it run a bit
    token.Cancel();      // Request cancellation
    worker.Join();
}
```

### Linked Tokens

Create a child token that cancels when the parent does:

```zia
bind CancelToken = Zanna.Threads.CancelToken;

var parentToken = CancelToken.New();
var childToken = CancelToken.Linked(parentToken);

parentToken.Cancel();  // Both parent and child are now cancelled
```

| Method/Property | Description |
|-----------------|-------------|
| `CancelToken.New()` | Create a new token |
| `.Cancel()` | Request cancellation |
| `.Reset()` | Reset to non-cancelled state |
| `.IsCancelled` | Returns true if cancelled |
| `.ThrowIfCancelled()` | Throws if cancelled |
| `CancelToken.Linked(parent)` | Create a child linked to parent |

---

## 24.10 Rate Limiting

### Debouncer

A `Debouncer` suppresses repeated signals that arrive within a quiet period. Only after the signals stop for the configured delay does the debouncer become "ready". This is useful for UI events like search-as-you-type, where you want to wait until the user stops typing.

```zia
bind Thread = Zanna.Threads.Thread;
bind Debouncer = Zanna.Threads.Debouncer;
bind Zanna.Terminal as Terminal;

func start() {
    var debounce = Debouncer.New(300);  // 300ms quiet period

    // Simulate rapid signals (like keystrokes)
    debounce.Signal();
    Thread.Sleep(100);
    debounce.Signal();
    Thread.Sleep(100);
    debounce.Signal();

    // Wait for the quiet period
    Thread.Sleep(400);

    if debounce.IsReady {
        Terminal.Say("Now execute the search!");
    }
}
```

| Method/Property | Description |
|-----------------|-------------|
| `Debouncer.New(delayMs)` | Create with quiet period |
| `.Signal()` | Register a signal (resets timer) |
| `.Reset()` | Reset state |
| `.Delay` | Configured delay in milliseconds |
| `.IsReady` | Whether the quiet period has elapsed |
| `.SignalCount` | Total signals received |

### Throttler

A `Throttler` limits how often an action can occur. At most one action per interval.

```zia
bind Thread = Zanna.Threads.Thread;
bind Throttler = Zanna.Threads.Throttler;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

func start() {
    var throttle = Throttler.New(1000);  // At most once per second

    for i in 0..10 {
        if throttle.TryAcquire() {
            Terminal.Say("Action executed at iteration " + Fmt.Int(i));
        } else {
            Terminal.Say("Throttled at iteration " + Fmt.Int(i));
        }
        Thread.Sleep(200);
    }
}
```

| Method/Property | Description |
|-----------------|-------------|
| `Throttler.New(intervalMs)` | Create with minimum interval |
| `.TryAcquire()` | Try to proceed (returns true if allowed) |
| `.Reset()` | Reset state |
| `.CanProceed` | Whether an action is allowed now |
| `.Count` | Total successful actions |
| `.Interval` | Configured interval in milliseconds |
| `.RemainingMs` | Milliseconds until next allowed action |

---

## 24.11 Scheduling

The `Scheduler` manages named tasks that should execute at scheduled times:

```zia
bind Thread = Zanna.Threads.Thread;
bind Scheduler = Zanna.Threads.Scheduler;
bind Box = Zanna.Core.Box;
bind Zanna.Terminal as Terminal;

func start() {
    var sched = Scheduler.New();

    // Schedule tasks by name with delay in milliseconds
    sched.Schedule("backup", 5000);      // Due in 5 seconds
    sched.Schedule("heartbeat", 1000);   // Due in 1 second

    // Poll loop
    while (sched.Pending > 0) {
        var dueTask = sched.Poll();  // Returns name of due task, or null
        if dueTask != null {
            Terminal.Say("Running: " + Box.ToStr(dueTask));
        }
        Thread.Sleep(100);
    }
}
```

| Method/Property | Description |
|-----------------|-------------|
| `Scheduler.New()` | Create a new scheduler |
| `.Schedule(name, delayMs)` | Schedule a named task |
| `.Cancel(name)` | Cancel a scheduled task |
| `.IsDue(name)` | Check if a task is due |
| `.Poll()` | Get next due task name (or null) |
| `.Clear()` | Cancel all tasks |
| `.Pending` | Number of scheduled tasks |

Scheduler operations are internally synchronized; multiple threads may schedule, cancel, poll, and clear the same instance.

---

## 24.12 Best Practices

### 1. Minimize Shared Mutable State

The primary source of concurrency bugs is shared mutable state. Prefer:

- **Channels** for communication between threads
- **Immutable data** passed to threads at creation time
- **`SafeI64`** for simple counters
- **`ConcurrentMap`/`ConcurrentQueue`** for shared collections

### 2. Always Join Threads

Unjoined threads can outlive the main function, leading to undefined behavior or resource leaks:

```zia
bind Thread = Zanna.Threads.Thread;

func doWork(arg: Any) {
    // Work goes here
}

func start() {
    // Bad: thread might not finish before program exits
    Thread.Start(&doWork, 0);

    // Good: always track and join
    var t = Thread.Start(&doWork, 0);
    t.Join();
}
```

### 3. Use StartSafe for Robustness

`Thread.StartSafe` catches errors in the thread body, preventing crashes and allowing error inspection:

```zia
bind Thread = Zanna.Threads.Thread;
bind Box = Zanna.Core.Box;
bind Zanna.Terminal as Terminal;

func riskyOperation(arg: Any) {
    var divisor = Box.ToI64(arg);
    var x = 1 / divisor;
}

func start() {
    var t = Thread.StartSafeOwned(&riskyOperation, Box.I64(0));
    t.SafeJoin();
    if t.HasError {
        Terminal.Say("Thread failed: " + t.Error);
    }
}
```

### 4. Prefer Thread Pools Over Raw Threads

Thread pools reuse threads, avoiding the overhead of creating and destroying OS threads:

```zia
bind Thread = Zanna.Threads.Thread;
bind Pool = Zanna.Threads.Pool;
bind Box = Zanna.Core.Box;

func processItem(arg: Any) {
    // Process one item
}

func start() {
// Bad: 1000 OS threads
    for i in 0..1000 {
        Thread.StartOwned(&processItem, Box.I64(i));
    }

// Good: 4 workers handling 1000 tasks
    var pool = Pool.New(4);
    for i in 0..1000 {
        Pool.Submit(pool, &processItem, Box.I64(i));
    }
    pool.Wait();
    pool.Shutdown();
}
```

### 5. Avoid Deadlocks

A *deadlock* occurs when two threads each wait for something the other holds. Rules to prevent deadlocks:

- Always acquire locks in the same order
- Use `TryEnter` / `TryEnterFor` with timeouts instead of blocking forever
- Keep critical sections (code between Enter/Exit) short
- Never call unknown code while holding a lock

### 6. Use Cancellation for Long Tasks

Cooperative cancellation via `CancelToken` lets you cleanly stop long-running work:

```zia
bind CancelToken = Zanna.Threads.CancelToken;

var token = CancelToken.New();

func processNextBatch() {
    // Process one batch
}

func processUntilCancelled(arg: Any) {
    while !token.IsCancelled {
        processNextBatch();
    }
}

// Later: request clean shutdown
token.Cancel();
```

---

## Summary

| Concept | Class | Use Case |
|---------|-------|----------|
| Threads | `Thread` | Independent execution paths |
| Mutual exclusion | `Monitor` | Protecting shared state |
| Atomic integers | `SafeI64` | Lock-free counters |
| Semaphore | `Gate` | Limiting concurrent access |
| Barrier | `Barrier` | Phase synchronization |
| Reader-writer lock | `RwLock` | Read-heavy shared data |
| Communication | `Channel` | Thread-safe message passing |
| Task execution | `Pool` | Reusable worker threads |
| Async results | `Promise` / `Future` | Deferred values |
| Combinators | `Async` | High-level async operations |
| Safe queue | `ConcurrentQueue` | Thread-safe FIFO |
| Safe map | `ConcurrentMap` | Thread-safe key-value store |
| Parallel loops | `Parallel` | Data parallelism |
| Cancellation | `CancelToken` | Cooperative task stopping |
| Rate limiting | `Debouncer` / `Throttler` | Controlling action frequency |
| Task scheduling | `Scheduler` | Time-based task management |

All concurrency types live under `Zanna.Threads`. Import them with:

```zia
bind Thread = Zanna.Threads.Thread;
bind Monitor = Zanna.Threads.Monitor;
bind Channel = Zanna.Threads.Channel;
// ... etc.
```

Concurrency is powerful but demands discipline. Start simple — use channels and thread pools before reaching for low-level primitives. Test under load. And remember: the best concurrent code is code where threads don't share state at all.

---

*We've covered the full breadth of Zanna's concurrency model. Part V takes you deeper into how Zanna itself works — the compiler, the VM, and how to write fast, testable, well-architected code.*

*[Continue to Part V: Mastery ->](../part5-mastery/25-how-zanna-works.md)*
