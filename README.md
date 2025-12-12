# AsyncTCPSock

AsyncTCPSock is a reimplementation of the API defined by [me-no-dev/AsyncTCP](https://github.com/me-no-dev/AsyncTCP) using high-level BSD sockets.
The result is usually higher throughput at the cost of increased latency.

This refactoring of the original [yubox-node-org](https://github.com/yubox-node-org/AsyncTCPSock) is an attempt to make the implementation clearer, easier, and faster.
Changes include:

- The use of the C++ standard library instead of Arduino's standard lib
- Using as many C++ best-practices as possible
  - Sometimes, the expected behavior unfortunately blocks this, e.g. raw `new` for accepted connections because they are supposed to be managed through the callbacks.
- Replacing the inheritance hierarchy for `Client`s and `Server`s with external polymorphism using `std::variant`
  - Currently, there still is a common base class for both to reduce code duplication, but the only virtual method left is the destructor.
- Moving SSL/TLS related code into a separate class
