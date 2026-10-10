// A "busy" worker: it never returns to its event loop, so the messages the page
// posts every 100ms are never dequeued. Each postMessage still serializes its
// ArrayBuffer into a C++ SerializedScriptValue and queues a task in the worker's
// C++ message queue (a Deque). Those copies pile up in C++ memory -- not as JS
// cells in either VM -- so a per-VM JS heap dump cannot see them. mya can.
while (true) {
  // Spin forever. Reading a volatile-ish value keeps the loop from being
  // optimized away; the worker never reaches onmessage.
  self.name;
}
