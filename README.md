First off: You can broadcast UDP, but that's not to connected clients. That's just... UDP.

Secondly, that link shows how to have a condition-variable (event) like interface in Asio. That's only a tiny part of your problem. You forgot about the big picture: you need to know about the set of open connections, one way or the other:

 0. e.g. keeping a container of session pointers (`weak_ptr`) to each connection
 0. each connection subscribing to a signal slot (e.g. [Boost Signals][1]).

Option 1. is great for performance, option 2. is better for flexibility (decoupling the event source from subscribers, making it possible to have  heterogenous subscribers, e.g. not from connections).

Because I think Option 1. is much simpler w.r.t to threading, better w.r.t. efficiency (you can e.g. serve all clients from one buffer without copying) and you probably don't need to doubly decouple the signal/slots, let me refer to an answer where I already showed as much for pure Asio (without Beast):

 * https://stackoverflow.com/questions/43239208/how-to-design-proper-release-of-a-boostasio-socket-or-wrapper-thereof/43243314#43243314

It shows the concept of a "connection pool" - which is essentially a thread-safe container of `weak_ptr<connection>` objects with some garbage collection logic.

It has a feature that allows you to perform an operation on all the connections that are still active:

```cpp
_pool.for_each_active([] (auto const& conn) {
    send_message(conn, hello_world_packet);
});
```

Where `for_each_active` is implemented in the pool like:

```cpp
template <typename F>
void for_each_active(F action) {
    auto locked = [=] {
        using namespace std;
        lock_guard<Mutex> lk(_mx);
        vector<Ptr> locked(_pool.size());
        transform(_pool.begin(), _pool.end(), locked.begin(), mem_fn(&WeakPtr::lock));
        return locked;
    }();

    for (auto const& p : locked)
        if (p) action(p);
}
```

> The thread-safety is opt-out (you can use a `null_mutex` to omit the synchronizations)

The full code is in the linked answer and in this gist: https://gist.github.com/sehe/979af25b8ac4fd77e73cdf1da37ab4c2


  [1]: http://www.boost.org/doc/libs/1_66_0/doc/html/signals2.html
