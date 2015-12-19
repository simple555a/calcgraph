#ifndef CALC_H
#define CALC_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <forward_list>
#include <unordered_map>
#include <memory>
#include <queue>
#include <thread>
#include <tuple>
#include <sstream>
#include <string>

#include <boost/intrusive_ptr.hpp>

namespace calcgraph {
    class Graph;
    class Work;
    template <typename>
    class Input;
    template <template <typename> class,
              template <template <typename> class, typename> class, typename,
              typename...>
    class Node;
    template <typename>
    class Connectable;
    template <template <typename> class,
              template <template <typename> class, typename> class, class...>
    class NodeBuilder;
    template <template <typename> class, typename>
    class Multiplexed;
    template <typename>
    class KeyedOutput;

    /**
     * @brief A less-than comparison of Work objects based on their ids
     */
    struct WorkQueueCmp {
        constexpr bool operator()(const Work *a, const Work *b) const;
    };

    template <typename VAL>
    class Storeable {
      public:
        virtual void store(VAL v) = 0;
    };

    /**
     * @brief An input policy that returns the latest value of the Input to the
     * Node to use when its eval() method is called.
     * @details This object is embedded in the Node class, and atomically stores
     * the current value of the input.
     *
     * @tparam VAL The type of the value stored - must be supported by
     * std::atomic.
     */
    template <typename VAL>
    class Latest final : public Storeable<VAL> {
      public:
        using input_type = VAL;
        using output_type = VAL;

        /**
         * @brief Atomically write the latest value into this class
         * @details Used by Upstream dependencies (e.g. those the Node is
         * connected to via the Connectable concept) to pass on new values for
         * the containing Node to evalute when its eval() method is called.
         */
        inline void store(input_type v) override {
            val.store(v, std::memory_order_release);
        }

        /**
         * @brief Used by the node to extract the stored value when its eval()
         * method is called.
         */
        inline output_type read() {
            return val.load(std::memory_order_consume);
        }

        /**
         * @brief Atomically exchange the stored value
         * @details Used by the OnChange propagation policy.
         */
        inline input_type exchange(input_type other) {
            return val.exchange(other, std::memory_order_acq_rel);
        }

        Latest(input_type initial = {}) noexcept : val(initial) {}
        Latest(const Latest &other) = delete;

      private:
        std::atomic<VAL> val;
    };

    /**
     * @brief A partial specialization of Latest for std::shared_ptr
     * @tparam VAL the type of the value the std::shared_ptr points to
     */
    template <typename VAL>
    class Latest<std::shared_ptr<VAL>> final
        : public Storeable<std::shared_ptr<VAL>> {
      public:
        using input_type = std::shared_ptr<VAL>;
        using output_type = std::shared_ptr<VAL>;

        inline void store(input_type v) override {
            std::atomic_store_explicit(&val, v, std::memory_order_release);
        }
        inline output_type read() {
            return std::atomic_load_explicit(&val, std::memory_order_acquire);
        }
        inline input_type exchange(input_type other) {
            return std::atomic_exchange_explicit(&val, other,
                                                 std::memory_order_acq_rel);
        }

        Latest(input_type initial = {}) noexcept : val(initial) {}
        Latest(const Latest &other) = delete;

      private:
        std::shared_ptr<VAL> val;
    };

    /**
     * @brief A partial specialization of Latest for boost::intrusive_ptr
     * @details This class keeps one reference to any value stored here, even if
     *the actual member type stored is a raw pointer.
     *
     * @tparam VAL the type of the value the boost::intrusive_ptr points to
     */
    template <typename VAL>
    class Latest<boost::intrusive_ptr<VAL>> final
        : public Storeable<boost::intrusive_ptr<VAL>> {
      public:
        using input_type = boost::intrusive_ptr<VAL>;
        using output_type = boost::intrusive_ptr<VAL>;

        /**
         * @brief Stores a new pointer
         * @details Uses exchange to decrement the reference count (and possibly
         * destroy) the value that was previously stored there when the returned
         * intrusive_ptr is destroyed as it goes out of scope at the end of this
         * function.
         */
        inline void store(input_type v) override { exchange(v); }

        /**
         * @brief Coverts the stored raw pointer back to an intrusive one,
         * without altering the reference count
         */
        inline output_type read() {
            return boost::intrusive_ptr<VAL>(
                val.load(std::memory_order_consume), false);
        }

        inline input_type exchange(input_type other) {
            return boost::intrusive_ptr<VAL>(
                val.exchange(other.detach(), std::memory_order_acq_rel), false);
        }

        Latest(input_type initial = {}) noexcept : val() { store(initial); }
        Latest(const Latest &other) = delete;

        /**
         * @brief Ensures we decrement the reference count of the object we've
         * got stored so we don't leak it.
         */
        ~Latest() {
            boost::intrusive_ptr<VAL>(val.load(std::memory_order_acquire),
                                      false);
        }

      private:
        std::atomic<VAL *> val;
    };

    /**
     * @brief An Input policy that accumulates any values fed to it and returns
     * them all to its containing Node as a std::forward_list
     * @details Useful if you can't afford to drop any values from upstream
     * dependencies
     *
     * @tparam VAL The type of the value stored - doesn't have to be atomic
     */
    template <typename VAL>
    class Accumulate final : public Storeable<VAL> {
      public:
        using input_type = VAL;
        using output_type = std::shared_ptr<std::forward_list<VAL>>;

        /**
         * @brief Atomically adds another value to the ones we've accumulated so
         * far
         */
        inline void store(input_type val) override {
            Element *e = new Element(std::move(val));
            while (true) {
                Element *snap = head.load(std::memory_order_acquire);
                e->next.store(snap, std::memory_order_release);
                if (head.compare_exchange_weak(snap, e))
                    return;
            }
        }

        /**
         * @brief Used by the node to extract any stored values its eval()
         * method is called.
         * @returns An empty list if there have been no new values since the
         * last time read() was called, or a std::forward_list of values in the
         * order that they were received.
         */
        inline output_type read() {
            auto ret = output_type(new std::forward_list<VAL>());

            Element *e = head.exchange(nullptr, std::memory_order_acq_rel);
            while (e != nullptr) {
                ret->push_front(e->val);
                Element *next = e->next.load(std::memory_order_consume);
                delete e;
                e = next;
            }

            return ret;
        }

        Accumulate(input_type initial = {}) noexcept : head() {}
        Accumulate(const Accumulate &other) = delete;

      private:
        struct Element final {
            std::atomic<Element *> next;
            VAL val;
            Element(VAL &&val) : next(), val(val) {}
        };

        std::atomic<Element *> head;
    };

    /**
     * @brief An input policy that converts many inputs into a std::vector of
     * values
     * @details [long description]
     *
     * @tparam VAL The type of the value stored - must be be atomic
     */
    template <typename VAL>
    class Variadic final : public Storeable<std::nullptr_t> {
      public:
        using input_type = std::nullptr_t;
        using output_type = std::shared_ptr<std::vector<VAL>>;
        using element_type = VAL;

        /**
         * @brief Should not be called directly, instead use new_input to get an
         * Input to append values to
         */
        inline void store(input_type val) override { std::abort(); }

        /**
         * @brief Used by the Node to extract the current list of latest values
         * from the inputs.
         * @details Called by Node when the lock is held, so accesses the inputs
         * vector in a thread-safe manner.
         * @returns A non-null (but possibly empty) std::vector, with one value
         * for each input created by new_input().
         */
        inline output_type read() {
            auto out = output_type(new std::vector<VAL>());
            for (auto i = inputs.begin(); i != inputs.end(); ++i) {
                out->push_back(i->read());
            }
            return out;
        }

        Variadic(input_type initial = nullptr) noexcept : inputs() {}
        Variadic(const Variadic &other) = delete;

      private:
        /**
         * @brief The Inputs, guarded by the containing Node's lock
         * @details A std::forward_list, not a std::vector as we can't move the
         * Latest objects (as std:atomics aren't copy-constructible), so we
         * can't resize a vector.
         */
        std::forward_list<Latest<VAL>> inputs;

        /**
         * @brief Creates a new input for the output vector and returns it.
         * @details Always created at the end. Only call when the containing
         * Node's lock is held.
         */
        inline Latest<VAL> &add_input(VAL initial = {}) {
            auto before_end = inputs.before_begin();
            for (auto &_ : inputs)
                ++before_end;
            inputs.emplace_after(before_end, initial);
            ++before_end; // point to the new element
            return *before_end;
        }

        /**
         * @brief Removes an input created by add_input from the list
         * @details Only call when the containing Node's lock is held.
         */
        inline void remove_input(Storeable<VAL> *it) {
            // Latest objects don't have equality methods, so just check if the
            // pointers match.
            inputs.remove_if([it](const Latest<VAL> &val) {
                return std::addressof(val) == it;
            });
        }

        template <template <typename> class,
                  template <template <typename> class, typename> class,
                  typename, typename...>
        friend class Node;
    };

    /**
     * @brief The state used for a single evalution of a Graph.
     */
    class WorkState final {
      public:
        /**
         * @brief add the given work item to the heap (to be processed later
         * this evalution) or the Graph's work_queue (to be processed next
         * evalution)
         *
         * @todo if we decide to push the Work onto the heap, we could "steal"
         * it from the Graph's work queue if it's on there too, so we don't
         * needlessly evaluate it a second time.
         */
        void add_to_queue(Work &work);

      private:
        /**
         * @brief the Work items to process this evaluation
         *
         * @todo use a fixed-size heap, and push any overflowing Work items back
         * on the Graph's work queue to evaluate later.
         */
        std::priority_queue<Work *, std::vector<Work *>, WorkQueueCmp> q;
        Graph &g;
        struct Stats *stats;
        friend class Graph;
        /**
         * @brief The Work.id of the Work item we're currently processing
         */
        uint32_t current_id;
        WorkState(Graph &g, struct Stats *stats) : g(g), stats(stats) {}
    };

    namespace flags {
        /**
         * @brief The lock bit in a Work's next pointer.
         */
        static const std::uintptr_t LOCK = 1;

        /**
         * A magic value for a Work's id field that means it won't ever get
         * scheduled for evaluation.
         */
        static const uint32_t DONT_SCHEDULE = 0;
    }

    /**
     * @brief A building block of the graph; either a raw input or code to
     * be evaluated.
     */
    class Work {
      public:
        /**
         * @brief The node's unique id
         * @details Uniqueness is per Graph, and this id is set by the Graph
         * object that created the Work.
         */
        const uint32_t id;

        virtual ~Work() {}

        /**
         * @brief Add this Work to the given Graph's work_queue
         * @details Returns when this Work is added to the work_queue of the
         * given Graph. Could return instantly if already scheduled.
         */
        void schedule(Graph &g);

        /**
         * @brief Actually do the work.
         */
        virtual void eval(WorkState &) = 0;

      protected:
        Work(uint32_t id) : id(id), refcount(0), next(0) {}
        Work(const Work &) = delete;
        Work &operator=(const Work &) = delete;

        friend class WorkState;
        template <typename>
        friend class KeyedOutput;

      private:
        // for boost's intrinsic_ptr
        std::atomic_uint_fast16_t refcount;
        friend inline void intrusive_ptr_add_ref(Work *w) { ++w->refcount; }
        friend inline void intrusive_ptr_release(Work *w) {
            if (--w->refcount == 0u) {
                delete w;
            }
        }

        /**
         * @page worklocking Locking a Work object
         *
         * We'll use the next pointer of a Work object to store two
         *orthogonal pieces of information. The LSB will store the "locked" - or
         *exclusive lock - flag, and the remaining bits will store the next link
         *in
         *the (intrusive) Graph.work_queue, or nullptr if this node isn't
         *scheduled. Note that the locked flag refers to the Work that contains
         *the next pointer, not the Work pointed to.
         */
      private:
        /**
         * @brief an intrinsic work queue for graph evaluation, headed by
         * Graph.work_queue
         */
        std::atomic<std::uintptr_t> next;

      public:
        /**
         * @brief Fetch and clear the Work item's next-pointer
         * @details This isn't the same as remove this Work item from the
         * Graph's work queue (if it was on it) as it doesn't clear the
         * intrinsic next pointer of any Work item pointing to this one.
         * @returns Extracts the pointer to the next node on the work_queue,
         * or
         * nullptr if this Work isn't on a Graph's work_queue.
         */
        Work *dequeue() {
            std::uintptr_t p =
                next.fetch_and(flags::LOCK, std::memory_order_acq_rel);
            return reinterpret_cast<Work *>(p & ~flags::LOCK);
        }

      protected:
        /**
         * @brief Tries to acquire the Work's exclusive lock
         * @details Not re-entrant
         * @return true if the lock was already taken, false if the lock was
         * successfully acquired.
         */
        bool trylock() {
            bool waslocked =
                next.fetch_or(flags::LOCK, std::memory_order_acq_rel) &
                flags::LOCK;
            return !waslocked;
        }

        /**
         * @brief Spins until the Work's exclusive lock is acquired
         * @details Not re-entrant
         */
        void spinlock() {
            while (!trylock()) {
                std::this_thread::yield();
            }
        }

        /**
         * @brief Release the Work's exclusive lock
         * @details ...by setting the LSB of the next pointer to zero. Only
         * call
         * if you already hold the lock, or the results are undefined.
         */
        void release() {
            next.fetch_and(~flags::LOCK, std::memory_order_release);
        }
    };

    /**
     * @brief A propagation policy that always recalculates downstream
     * dependencies
     * @tparam RET The type of the values calcuated by the Node this policy
     * applies to
     */
    template <typename RET>
    struct Always final {
        inline constexpr bool push_value(RET) { return true; }
        inline constexpr bool notify() const { return true; }
    };

    /**
     * @brief A propagation policy that recalculates downstream dependencies
     * only if the Node's output changes (according to the != operator)
     * @details This method isn't thread-safe, but doesn't need to be as it's
     * only called by Node.eval() when the calculation lock is held. This policy
     * stores the last output value in the Node, so increases the size of the
     * templated Node class and potentially uses even more memory (e.g. if the
     * output is a std::shared_ptr to a large object).
     * @tparam RET The type of the values calcuated by the Node this policy
     * applies to
     */
    template <typename RET>
    struct OnChange final {
        inline constexpr bool notify() const { return true; }
        inline bool push_value(RET latest) {
            return last.exchange(latest) != latest;
        }

      private:
        Latest<RET> last;
    };

    /**
     * @brief A specialization of OnChange for std::shared_ptrs that compares
     * what the pointers point at
     */
    template <typename RET>
    struct OnChange<std::shared_ptr<RET>> final {
        inline constexpr bool notify() const { return true; }
        inline bool push_value(std::shared_ptr<RET> latest) {
            auto previous = last.exchange(latest);
            if (latest && previous)
                return *latest != *previous;
            else
                return latest != previous;
        }

      private:
        Latest<std::shared_ptr<RET>> last;
    };

    /**
     * @brief A propagation policy that passes on values but doesn't schedule
     * downstream Work items to be calculated
     * @details Useful for circular references
     * @tparam RET The type of the values calcuated by the Node this policy
     * applies to
     */
    template <typename RET>
    struct Weak final {
        inline constexpr bool notify() const { return false; }
        inline constexpr bool push_value(RET latest) { return true; }
    };

    /**
     * @brief A concept for "something you can connect an Input to"
     * @tparam RET The type of the values consumed (and so the type of values
     * the Input should provide)
     */
    template <typename RET>
    class Connectable {
      public:
        /**
         * @brief Connect the given Input to this object.
         * @details Must be thread-safe and can be called multiple times on any
         * given object.
         */
        virtual void connect(Input<RET>) = 0;

        /**
         * @brief Disconnects the given Input from this object.
         * @details Must be thread-safe and has no effect if the given Input
         * wasn't already connected
         */
        virtual void disconnect(Input<RET>) = 0;

        virtual ~Connectable() {}
    };

    /**
     * @brief User API for a Multiplexed output policy, for use in functions
     *passed to Node.embed
     * @details Node.embed allows the given function access to the
     *currently-executing Node's output data structures without additional
     *locking (which is especially useful as the Node locks aren't re-entrant).
     *This interface is the API to a Multiplexed policy, and is only valid
     *during execution of the function it was passed to.
     *
     * @tparam KEY The type used to uniquely identify each of the Muliplexed
     *Inputs
     * @tparam VALUE The types of values fed to each keyed Input
     */
    template <typename KEY, typename VALUE>
    class KeyedConnectable
        : public Connectable<std::shared_ptr<std::pair<KEY, VALUE>>> {
      public:
        /**
         * @brief Get the output associated with the given key
         * @details A pointer to the Node's (unlocked) data structure for the
         * key. This pointer is also only valid during the execution of the
         * function it was passed to.
         */
        virtual Connectable<VALUE> *keyed_output(KEY) = 0;
        virtual ~KeyedConnectable() {}
    };

    /**
     * @brief The internal data structure for a Node that manages a list of
     *Inputs.
     * @details This class is not thread-safe - access is governed by the
     *containing Node.
     *
     * @tparam PROPAGATE The propagation policy to use
     * @tparam RET The type of value the managed Inputs can consume.
     */
    template <template <typename> class PROPAGATE, typename RET>
    class SingleList final : public Connectable<RET> {
      public:
        using output_type = RET;
        using key_type = std::nullptr_t;
        using value_type = std::nullptr_t;
        /**
         * @brief The class to use to represent this object when it's passed to
         * a Node.embed'ed function.
         */
        using interface_type = Connectable<output_type>;

      private:
        using embed_type =
            const std::function<void(output_type, interface_type &)>;

      public:
        /**
         * @brief Adds the given Input to the managed list, even if it is
         * already present.
         */
        inline void connect(Input<output_type> a) override {
            dependents.push_back(a);
        }

        /**
         * @brief Removes all occurances of the given Input from the managed
         * list.
         */
        inline void disconnect(Input<output_type> a) override {
            for (std::size_t i = 0; i < dependents.size(); ++i) {
                if (dependents[i] == a) {
                    // slightly less shuffling
                    std::swap(dependents[i], dependents.back());
                    dependents.pop_back();
                }
            }
        }

        /**
         * @brief Pass the given value to each of the Inputs (if allowed by the
         * propagation policy) and schedule them (also if allowed).
         */
        inline void propagate(RET &&val, WorkState &ws) {
            if (!propagation_policy.push_value(val))
                return;

            // accessing i by index as embedded Inputs may modify the list,
            // invalidating any iterators
            for (std::size_t i = 0; i < dependents.size(); ++i) {
                dependents[i].in->store(val);
                if (propagation_policy.notify() && dependents[i].ref) {
                    ws.add_to_queue(*dependents[i].ref);
                }
            }
        }

        /**
         * @brief Attach the given function to an Input and add that Input to
         *the list.
         * @see Node.embed
         *
         * @return The created Input, which you can pass to disconnect.
         */
        inline Input<output_type> embed(embed_type &&fn) {
            class Embed final : public Storeable<output_type>, public Work {

                embed_type fn;
                interface_type *output;

                Embed(embed_type &&fn, interface_type *output) noexcept
                    : fn(fn),
                      output(output),
                      Work(flags::DONT_SCHEDULE) {}
                Embed(const Embed &other) = delete;
                friend class SingleList<PROPAGATE, RET>;

              public:
                inline void store(output_type v) { fn(v, *output); }
                void eval(WorkState &) { std::abort(); }
            };
            auto ret = new Embed(std::move(fn), this);
            return Input<output_type>(*ret, ret);
        }

        SingleList() noexcept : dependents(), propagation_policy() {}

      private:
        /**
         * @brief Downstream dependencies
         * @details Not threadsafe so controlled by the containing Work's next
         * LSB locking mechanism
         */
        std::vector<Input<output_type>> dependents;

        /**
         * @brief The propagation policy to use for these inputs
         */
        PROPAGATE<RET> propagation_policy;
    };

    /**
     * @brief An output policy that wraps another policy, feeding the iterable
     *output of the Node's function to its delegate one element at a time.
     * @details Useful when the containing Node processes a batch of values
     *(maybe from an Accumulate input policy) but downstream Nodes can only
     *process a single element at a time.
     *
     * @tparam OUTPUT The output policy to wrap
     */
    template <template <template <typename> class, typename> class OUTPUT>
    struct MultiValued final {
        template <template <typename> class PROPAGATE, typename RET>
        class type final
            : public Connectable<typename OUTPUT<
                  PROPAGATE,
                  typename RET::element_type::value_type>::output_type> {
          private:
            using single_type = typename RET::element_type::value_type;

          public:
            using output_type =
                typename OUTPUT<PROPAGATE, single_type>::output_type;
            using key_type = typename OUTPUT<PROPAGATE, single_type>::key_type;
            using value_type =
                typename OUTPUT<PROPAGATE, single_type>::value_type;
            /**
             * @brief The class to use to represent this object when it's passed
             * to a Node.embed'ed function.
             */
            using interface_type =
                typename OUTPUT<PROPAGATE, single_type>::interface_type;

            inline void connect(Input<output_type> a) { output.connect(a); }

            inline void disconnect(Input<output_type> a) {
                output.disconnect(a);
            }

            /**
             * @brief Iterate over the given value using std::begin and std::end
             * to get iterators, passing each value to the delegate output
             * policy.
             */
            inline void propagate(RET &&val, WorkState &ws) {
                for (auto i = std::begin(*val); i != std::end(*val); i++) {
                    output.propagate(std::move(*i), ws);
                }
            }

            inline KeyedOutput<value_type>
            keyed_output(key_type key, boost::intrusive_ptr<Work> ref) {
                return output.keyed_output(key, ref);
            }

            inline Input<output_type> embed(
                const std::function<void(output_type, interface_type &)> &&fn) {
                return output.embed(std::move(fn));
            }

          private:
            OUTPUT<PROPAGATE, single_type> output;
        };
    };

    /**
     * @brief A helper for NodeBuilder.connect, to indicate that the given
     * Input shouldn't be connected to anything.
     * @return An appropriately-cast nullptr that can be passed to connect
     * or NodeBuilder.connect
     */
    template <typename RET>
    inline Connectable<RET> *unconnected() {
        return static_cast<Connectable<RET> *>(nullptr);
    }

    /**
     * @brief Connects the Input to the target Connectable
     * @param to The (possibly-nullptr) Connectable to add this input to
     * @param from A non-nullptr Input to connect
     * @tparam RET the type of the Input and target
     */
    template <typename RET>
    inline void connect(Connectable<RET> *to, Input<RET> from) {
        if (to)
            to->connect(from);
    }

    /**
     * @brief A Connectable implementation returned by Node.keyed_output.
     * @details This class is thread-safe and can be passed across threads and
     *stored on the heap. It keeps a reference to the Node that created it so
     *it'll never outlive the Node it points to. As Node locks aren't
     *re-entrant, it can't be used from a function passed to its Node's
     *Node.embed method.
     *
     * @tparam value_type The type of values this class can accept.
     */
    template <typename value_type>
    class KeyedOutput final : public Connectable<value_type> {

      public:
        KeyedOutput(const KeyedOutput &other) noexcept
            : delegate(other.delegate),
              ref(other.ref) {}
        KeyedOutput(KeyedOutput &&other) noexcept
            : delegate(std::move(other.delegate)),
              ref(std::move(other.ref)) {}
        KeyedOutput &operator=(const KeyedOutput &other) noexcept {
            delegate = other.delegate;
            ref = other.ref;
            return *this;
        }
        KeyedOutput &operator=(KeyedOutput &&other) noexcept {
            delegate = std::move(other.delegate);
            ref = std::move(other.ref);
            return *this;
        }

        void connect(Input<value_type> a) override {
            ref->spinlock();
            delegate->connect(a);
            ref->release();
        }

        void disconnect(Input<value_type> a) override {
            ref->spinlock();
            delegate->disconnect(a);
            ref->release();
        }

      private:
        KeyedOutput(Connectable<value_type> &delegate,
                    boost::intrusive_ptr<Work> ref) noexcept
            : delegate(&delegate),
              ref(ref) {}
        template <template <typename> class, typename>
        friend class Multiplexed;

        Connectable<value_type> *delegate;

        /**
         * To make sure the containing Node isn't destroyed while we're
         * still alive
         */
        boost::intrusive_ptr<Work> ref;
    };

    /**
     * @brief An output policy that passes values to different Inputs
     *depending on part of the value (the key, i.e. what std::get<0> returns).
     * @details Use the keyed_output function to get a Connectable for the given
     *key. Any key-value pairs passed to this Multiplexed instance for keys that
     *haven't been passed (so far) to keyed_output will be passed as std::pairs
     *to the default output. Note that all values for these "unmatched" keys
     *will be sent, so even if you use this functionality to build parts of the
     *calculation graph to deal with new keys you won't miss values sent while
     *this node is being eval()'ed.
     *
     * @tparam RET A templatized std::pair, where the left side is the key
     *and the right side is the value to be passed to downstream nodes.
     */
    template <template <typename> class PROPAGATE, typename RET>
    class Multiplexed final : public Connectable<std::shared_ptr<RET>> {
      public:
        /**
         * @brief The type of values that aren't associated with a key
         * @details Needs to be a shared pointer so we can pass to
         * downstream nodes atomically
         */
        using output_type = std::shared_ptr<RET>;
        using key_type = typename RET::first_type;
        using value_type = typename RET::second_type;
        /**
         * @brief The class to use to represent this object when it's passed to
         * a Node.embed'ed function.
         */
        using interface_type = KeyedConnectable<key_type, value_type>;

      private:
        using embed_type =
            const std::function<void(output_type, interface_type &)>;

      public:
        /**
         * @brief Connect an Input to the unkeyed output values.
         */
        inline void connect(Input<output_type> a) { unkeyed.connect(a); }

        /**
         * @brief Disconnect an Input from the unkeyed output values.
         */
        inline void disconnect(Input<output_type> a) { unkeyed.disconnect(a); }

        /**
         * @brief Pass the given value of the key-value std::pair to the
         * appropriate Inputs, or pass the whole pair to the unkeyed Inputs if
         * no Input keyed_output has never been called for the key.
         */
        inline void propagate(RET &&val, WorkState &ws) {
            auto found = keyed.find(val.first);
            if (found == keyed.end()) {
                // pass the whole thing to the unkeyed output
                output_type on_heap(new RET(val));
                unkeyed.propagate(std::move(on_heap), ws);
            } else {
                found->second.propagate(std::move(val.second), ws);
            }
        }

        /**
         * @brief Return a Connectable directly connected to a particular key.
         *
         * @param ref we'll use this to lock the Work object while we inspect
         *the lists, as all our internal data structures are protected by the
         *containing Node's lock.
         */
        inline KeyedOutput<value_type>
        keyed_output(key_type key, boost::intrusive_ptr<Work> ref) {
            return KeyedOutput<value_type>(lookup(key), ref);
        }

        /**
         * @brief Attach the given function to an Input and add that Input to
         *the "unkeyed" list.
         * @see Node.embed
         *
         * @return The created Input, which you can pass to disconnect.
         */
        inline Input<output_type> embed(embed_type &&fn) {
            class Embed final : public Storeable<output_type>,
                                public Work,
                                public interface_type {

                embed_type fn;
                Multiplexed<PROPAGATE, RET> *output;

                Embed(embed_type &&fn,
                      Multiplexed<PROPAGATE, RET> *output) noexcept
                    : fn(fn),
                      output(output),
                      Work(flags::DONT_SCHEDULE) {}
                Embed(const Embed &other) = delete;
                friend class Multiplexed<PROPAGATE, RET>;

              public:
                inline void store(output_type v) { fn(v, *this); }
                void eval(WorkState &) { std::abort(); }
                void connect(Input<output_type> a) { output->connect(a); }
                void disconnect(Input<output_type> a) { output->disconnect(a); }
                Connectable<value_type> *keyed_output(key_type key) {
                    return &output->lookup(key);
                }
            };
            auto ret = new Embed(std::move(fn), this);
            return Input<output_type>(*ret, ret);
        }

      private:
        std::unordered_map<key_type, SingleList<PROPAGATE, value_type>> keyed;
        SingleList<PROPAGATE, output_type> unkeyed;

        Connectable<value_type> &lookup(key_type key) {
            auto found = keyed.emplace(std::piecewise_construct,
                                       std::forward_as_tuple(key),
                                       std::forward_as_tuple());
            return found.first->second;
        }
    };

    /**
     * @brief Statistics for a single evaluation of the calculation graph.
     */
    struct Stats final {
        /** @brief how many items were taken off the work queue */
        uint16_t queued;
        /** @brief how many Nodes were eval()'ed */
        uint16_t worked;
        /**
         * @brief how many Nodes were added to this evaluation's heap
         * multiple times (as they were dependent on more than one queued or
         * dependent Node)
         */
        uint16_t duplicates;
        /**
         * @brief how many dependencies were pushed back on to the Graph's
         * work_queue to be evaluted next time
         */
        uint16_t pushed_graph;
        /**
         * @brief how many dependencies were pushed onto this evaluation's
         * work heap to be evaluated in topological order
         */
        uint16_t pushed_heap;

        operator std::string() const {
            std::ostringstream out;
            out << "queued: " << queued;
            out << ", worked: " << worked;
            out << ", duplicates: " << duplicates;
            out << ", pushed_graph: " << pushed_graph;
            out << ", pushed_heap: " << pushed_heap;
            return out.str();
        }
    };

    /**
     * @brief A helper member to zero out a Stats object
     */
    static const struct Stats EmptyStats {};

    /**
     * @brief The calcuation-graph-wide state
     * @details This class is the only way to make calculation nodes in the
     * graph, and is in charge of the work_queue, a intrinsic singly-linked
     * list of Work that needs re-evaluating due to upstream changes.
     */
    class Graph final {
      public:
        Graph() : ids(1), tombstone(), work_queue(&tombstone) {}

        /**
         * @brief Run the graph evaluation to evalute all Work items on the
         * work_queue, and all items recursively dependent on them (at
         *least, as determined by each Node's propagation policy).
         * @details Doesn't release the locks we have on the Work items in
         *the queue, as we'll just put them in a heap.
         *
         * @return true iff any Work items were eval'ed
         */
        bool operator()(struct Stats * = nullptr);

        /**
         * @brief Creates a builder object for Nodes
         * @details Sets the default propagation policy as Always.
         */
        NodeBuilder<Always, SingleList> node();

        ~Graph() {
            auto head =
                work_queue.exchange(&tombstone, std::memory_order_acq_rel);
            Work *w = head;
            while (w != &tombstone) {
                Work *next = w->dequeue(); // read before deleting
                intrusive_ptr_release(w);
                w = next;
            }
        }

      private:
        /**
         * @brief The source of Work ids
         */
        std::atomic<uint32_t> ids;

        /**
         * @brief The head of the work queue.
         */
        std::atomic<Work *> work_queue;

        class Tombstone : public Work {
          public:
            void eval(WorkState &) { std::abort(); }

          private:
            Tombstone() : Work(flags::DONT_SCHEDULE) {}
            friend class Graph;
        };

        /**
         * @brief The tail of the work queue.
         */
        Tombstone tombstone;

        friend class WorkState;
        template <typename>
        friend class Input;
        template <template <typename> class,
                  template <template <typename> class, typename> class,
                  class...>
        friend class NodeBuilder;
        friend class Work;

        template <typename... INPUTS, std::size_t... I>
        void connectall(std::index_sequence<I...>,
                        std::tuple<Connectable<INPUTS> *...> tos,
                        std::tuple<Input<INPUTS>...> froms) {
            int forceexpansion[] = {
                0, (connect(std::get<I>(tos), std::get<I>(froms)), 0)...};
        }
    };

    /**
     * @brief An input to a calculation Node
     * @details Used for putting external data into the calcuation graph,
     *and connecting calculation nodes to each other.
     *
     * @tparam INPUT the type of the values this input accepts.
     */
    template <typename INPUT>
    class Input final {
      public:
        /**
         * @brief Sets the input to an externally-provided value, and
         *schedules the Node we're an Input of on the Graph's work_queue for
         * re-evaluation.
         *
         * @param graph The owner of the work queue to add the Input's Node
         *to
         * @param v The new value to set the Input to
         */
        void append(Graph &graph, INPUT v) {
            in->store(v);
            if (ref) {
                ref->schedule(graph);
            }
        }

        Input(std::shared_ptr<Storeable<INPUT>> in) noexcept : in(in.get()),
                                                               ref() {}
        Input(Storeable<INPUT> *in) noexcept : in(in), ref() {}
        Input(Storeable<INPUT> &in) noexcept : in(&in), ref() {}
        Input(const Input &other) noexcept : in(other.in), ref(other.ref) {}
        Input(Input &&other) noexcept : in(std::move(other.in)),
                                        ref(std::move(other.ref)) {}
        Input &operator=(const Input &other) noexcept {
            in = other.in;
            ref = other.ref;
            return *this;
        }
        Input &operator=(Input &&other) noexcept {
            in = std::move(other.in);
            ref = std::move(other.ref);
            return *this;
        }

        /**
         * @brief Equality semantics, based on what it's an Input to, but
         * not what the current value is.
         */
        inline bool operator==(const Input &other) const {
            return in == other.in;
        }
        inline bool operator!=(const Input &other) const {
            return !(*this == other);
        }

      private:
        Storeable<INPUT> *in;

        /**
         * @brief A ref-counted pointer to the owner of the 'in' Latest, so
         * it doesn't get freed while we're still alive
         */
        boost::intrusive_ptr<Work> ref;

        Input(Storeable<INPUT> &in, boost::intrusive_ptr<Work> ref)
            : in(&in), ref(ref) {}

        template <template <typename> class,
                  template <template <typename> class, typename> class,
                  typename, typename...>
        friend class Node;
        template <typename>
        friend class Accumulator;
        template <template <typename> class, typename>
        friend class SingleList;
        template <template <typename> class, typename>
        friend class Multiplexed;
    };

    /**
     * @brief A Work item that evaluates a function, and propages the
     *results to any connected Inputs.
     * @details The key part of the calculation graph - constructed using a
     * NodeBuilder obtained from Graph.node().
     * @todo This class doesn't support FN functions with a void return type.
     *
     * @tparam PROPAGATE The propagation policy (e.g. Always or OnChange),
     *used to decide whether to notify connected Inputs after the FN function is
     *evaluated. An instance of the policy must have a push_value method, to
     *decide whether to even tell downstream nodes about the new value, and a
     *notify method, to decide whether to schedule downstream nodes that we just
     *pushed values to for re-evaluation.
     * @tparam FN The function to evaluate. Must be able to operate on
     * default-constructed INPUTS arguments, as it may be invoked before
     * upstream dependencies pass on their values
     * @tparam INPUTS The input policies for the parameters of the FN
     *function. You can get the input type of the parameter using INPUTS::type.
     */
    template <template <typename> class PROPAGATE,
              template <template <typename> class, typename> class OUTPUT,
              typename FN, typename... INPUTS>
    class Node final
        : public Work,
          public Connectable<typename OUTPUT<
              PROPAGATE, std::result_of_t<FN(
                             typename INPUTS::output_type...)>>::output_type> {
      public:
        using RET =
            typename std::result_of_t<FN(typename INPUTS::output_type...)>;

      private:
        using output_type = typename OUTPUT<PROPAGATE, RET>::output_type;
        using interface_type = typename OUTPUT<PROPAGATE, RET>::interface_type;

      public:
        /**
         * @brief Get an Input object associated with the N'th argument of
         *this Node's function FN
         * @details Pass this object to a Connectable object so this Node is
         * evaluated whenever that Connectable changes, or set values on the
         * Input (i.e. pass values to the FN function) directly using
         * Input.append().
         *
         * @tparam N which function argument to get
         */
        template <std::size_t N>
        Input<typename std::tuple_element_t<
            N, typename std::tuple<INPUTS...>>::input_type>
        input() {
            return Input<typename std::tuple_element_t<
                N, typename std::tuple<INPUTS...>>::input_type>(
                std::get<N>(inputs), boost::intrusive_ptr<Work>(this));
        }

        /**
         * @brief Return a Tuple of all the Inputs to this function.
         * @details Equivalent to calling input() for the N arguments of the
         * FN function
         */
        std::tuple<Input<typename INPUTS::input_type>...> inputtuple() {
            return inputtuple_fn(std::index_sequence_for<INPUTS...>{});
        }

        /**
         * @brief Add a new Input to the (variadic) the N'th argument of
         *this Node's function FN
         * @details Pass this object to a Connectable object so this Node is
         * evaluated whenever that Connectable changes, or set values on the
         * Input (i.e. pass values to the FN function) directly using
         * Input.append(). Note that this method won't schedule the node for
         * evaluation, so if you want the expanded vector to be processed
         *you should schedule the Node directly.
         *
         * @tparam N which function argument to get; it must have a Variadic
         * input policy
         */
        template <std::size_t N>
        Input<typename std::tuple_element_t<
            N, typename std::tuple<INPUTS...>>::element_type>
        variadic_add(
            typename std::tuple_element_t<
                N, typename std::tuple<INPUTS...>>::element_type initial = {}) {
            spinlock();
            auto ret = Input<typename std::tuple_element_t<
                N, typename std::tuple<INPUTS...>>::element_type>(
                std::get<N>(inputs).add_input(initial),
                boost::intrusive_ptr<Work>(this));
            release();
            return ret;
        }

        /**
         * @brief Add a new Input to the (variadic) the N'th argument of
         *this Node's function FN
         * @details Pass this object to a Connectable object so this Node is
         * evaluated whenever that Connectable changes, or set values on the
         * Input (i.e. pass values to the FN function) directly using
         * Input.append(). Note that this method won't schedule the node for
         * evaluation, so if you want the reduced vector to be processed you
         * should schedule the Node directly. The Input must not be used
         *after passed to this method.
         *
         * @tparam N which function argument to get; it must have a Variadic
         * input policy
         */
        template <std::size_t N>
        void variadic_remove(Input<typename std::tuple_element_t<
            N, typename std::tuple<INPUTS...>>::element_type> &input) {
            spinlock();
            std::get<N>(inputs).remove_input(input.in);
            release();
        }

        /**
         * @brief Connect an Input to the output of this Node's FN function
         * @details Newly-calculated values will be fed to the Input
         * according
         * to this Node's PROPAGATE propagation policy
         */
        void connect(Input<output_type> a) override {
            spinlock();
            output.connect(a);
            release();
        }

        /**
         * @brief Disconnect an Input from the output of this Node's FN
         * function
         * @details Has no effect if the given Input wasn't connected in the
         * first place.
         */
        void disconnect(Input<output_type> a) override {
            spinlock();
            output.disconnect(a);
            release();
        }

        /**
         * @brief If this Node's output policy supports keyed outputs, this
         *method will return a thread-safe Connectable for the given key.
         * @details The returned class is thread-safe and can be passed across
         *threads and stored on the heap. It keeps a reference to this Node so
         *it'll never outlive it. As Node locks aren't re-entrant, it can't be
         *used from a function passed to its Node's Node.embed method.
         */
        template <template <template <typename> class, typename>
                  class O = OUTPUT>
        typename std::enable_if_t<
            std::is_same<O<PROPAGATE, RET>, OUTPUT<PROPAGATE, RET>>::value,
            KeyedOutput<typename OUTPUT<PROPAGATE, RET>::value_type>>
        keyed_output(typename O<PROPAGATE, RET>::key_type key) {
            spinlock();
            auto ret = output.keyed_output(key, this);
            release();
            return ret;
        }

        /**
         * @brief Call the FN function on the current INPUTS values, and
         * propagate the result to any connected Inputs
         * @details Propagation is controlled by the PROPAGATE propagation
         * policy. This function exclusively locks the Node so only one
         * thread can call this method at once. If a thread tries to call eval()
         * when the Node is locked, it will instead put this Node back on the
         * WorkState Graph's work_queue.
         */
        void eval(WorkState &ws) override {
            if (!this->trylock()) {
                // another calculation in progress, so put us on the work
                // queue (which will change the next pointer to the next node in
                // the work queue, not this)
                ws.add_to_queue(*this);
                return;
            }

            // there's a race condition here: this Node can be put on the
            // work queue, so if the inputs change we'll get rescheduled and
            // re-ran. We only snap the atomic values in the next statement, so
            // we could pick up newer values than the ones that triggered the
            // recalculation of this Node, so the subsequent re-run would
            // be unnecessary. See the OnChange propagation policy to
            // mitagate this (your function should be idempotent!).

            RET val = call_fn(std::index_sequence_for<INPUTS...>{});
            output.propagate(std::move(val), ws);

            this->release();
        }

        /**
         * @brief Attach a function to this Node as an Input connected to this
         *Node's output policy.
         * @details Useful for attaching debugging or telemetry logic, or
         *dynamically modifying the Node's output policy. The given function is
         *attached to an Input and connected to this Node using connect, so is
         *executed after the Node's main function is executed (i.e. when the
         *results of the Node's function are passed to the output policy).
         *
         * @return An Input that can be used to remove this function from the
         *Node - pass it as the argument to disconnect.
         */
        Input<output_type>
        embed(std::function<void(output_type, interface_type &)> fn) {
            Input<output_type> ret = output.embed(std::move(fn));
            connect(ret);
            return ret;
        }

      private:
        /**
         * The policy on what connected values to pass FN outputs to
         */
        OUTPUT<PROPAGATE, RET> output;

        const FN fn;
        std::tuple<INPUTS...> inputs;

        Node(uint32_t id, const FN fn,
             std::tuple<typename INPUTS::input_type...> initials)
            : Work(id), fn(fn), inputs(initials) {}
        friend class Graph;

        template <std::size_t... I>
        inline RET call_fn(std::index_sequence<I...>) {
            return fn(std::get<I>(inputs).read()...);
        }
        template <std::size_t... I>
        inline auto inputtuple_fn(std::index_sequence<I...>) {
            return std::make_tuple<Input<typename INPUTS::input_type>...>(
                input<I>()...);
        }

        template <template <typename> class,
                  template <template <typename> class, typename> class,
                  class...>
        friend class NodeBuilder;
    };

    /**
     * @brief A builder-pattern object for constructing Nodes
     * @details Can be reused to create multiple Nodes. Arguments can be
     * passed either to successive calls to latest and accumulate (if you want
     * to specify the input policy) or passed all at once to connect (if you
     * just need the default input policy, Latest), or a mixture (in which case
     * the parameters passed to connect come after those passed to latest and
     * accumulate).
     *
     * @tparam PROPAGATE the propagation policy used by the Nodes it
     *constructs
     * @tparam INPUTS the input policies of the first N arguments of the
     *function passed to connect
     */
    template <template <typename> class PROPAGATE,
              template <template <typename> class, typename> class OUTPUT,
              class... INPUTS>
    class NodeBuilder final {
      public:
        /**
         * @brief Change the propagation policy for the Nodes the builder
         * constructs
         * @tparam NEWPROPAGATE the new propagation policy to use
         * @return A new NodeBuilder object
         */
        template <template <typename> class NEWPROPAGATE>
        auto propagate() {
            return NodeBuilder<NEWPROPAGATE, OUTPUT, INPUTS...>(g, connected,
                                                                initials);
        }

        template <template <template <typename> class, typename>
                  class NEWOUTPUT>
        auto output() {
            return NodeBuilder<PROPAGATE, NEWOUTPUT, INPUTS...>(g, connected,
                                                                initials);
        }

        /**
         * @brief Add an argument with an Accumulate input policy
         */
        template <typename VAL>
        auto accumulate(Connectable<VAL> *arg) {
            return doconnect<Accumulate, VAL>(arg);
        }

        /**
         * @brief Add an argument with a Latest input policy
         *
         * @param initial The initial value for this argument - if no new values
         *are passed via an Input this value will be used by the Node when its
         *function is called. If not given the initial argument will be a
         *default-constructed type.
         */
        template <typename VAL>
        auto latest(Connectable<VAL> *arg, VAL initial = {}) {
            return doconnect<Latest, VAL>(arg, std::move(initial));
        }

        /**
         * @brief Add an argument with a Latest input policy, with the given
         * initial value
         * @details Useful for implementing constant inputs to the Graph.
         */
        template <typename VAL>
        auto initialize(VAL initial) {
            return doconnect<Latest, VAL>(
                static_cast<Connectable<VAL> *>(nullptr), std::move(initial));
        }

        /**
         * @brief Add an argument with a Latest input policy and the default
         * initial value
         * @details Useful for having a placeholder Input that you'll connect
         * later.
         */
        template <typename VAL>
        auto unconnected() {
            return doconnect<Latest, VAL>(
                static_cast<Connectable<VAL> *>(nullptr));
        }

        /**
         * @brief Add an argument with a Variadic input policy
         */
        template <typename VAL>
        auto variadic() {
            return doconnect<Variadic, VAL>(
                static_cast<Connectable<std::nullptr_t> *>(nullptr));
        }

        /**
         * @brief Build a Node
         * @details Any extract Connectable arguments can't be given default
         *values using this method - use latest(...) instead.
         *
         * @param fn The function the node should execute. The
         *newly-constructed node will be schedule for evaluation on the Graph,
         *so this function should be able to accept default-constructed values
         *of its Inputs (as it may be evaluated before upstream dependencies
         *pass on values to it)
         * @param args Any additional Connectable objects (including
         * calcgraph::unconnected() objects) or appropriately-cast nullptrs
         *to provide values for the calculation function. The Graph will
         * recalcuate the Node when any one of these Connectable objects
         *pass on a new value to the Node (coalescing where possible). This
         *arguments are appended to those passed to this NodeBuilder via calls
         *to accumulate or latest.
         * @tparam VALS The types of the function's arguments, not the input
         * policy.
         * @return A new Node
         */
        template <typename FN, typename... VALS>
        auto connect(const FN fn, Connectable<VALS> *... args) {

            // first, make the node
            std::tuple<VALS...> newinitials{};
            auto finalinitials =
                std::tuple_cat(initials, std::move(newinitials));
            auto node = boost::intrusive_ptr<
                Node<PROPAGATE, OUTPUT, FN, INPUTS..., Latest<VALS>...>>(
                new Node<PROPAGATE, OUTPUT, FN, INPUTS..., Latest<VALS>...>(
                    g.ids++, fn, finalinitials));

            // next, connect any given inputs
            auto newargs =
                std::make_tuple<Connectable<VALS> *...>(std::move(args)...);
            auto finalargs = std::tuple_cat(connected, std::move(newargs));
            g.connectall(std::index_sequence_for<INPUTS..., VALS...>{},
                         finalargs, node->inputtuple());

            // finally schedule it for evaluation
            node->schedule(g);
            return node;
        }

      private:
        using STORED =
            std::tuple<Connectable<typename INPUTS::input_type> *...>;
        using INITIALS = std::tuple<typename INPUTS::input_type...>;

        Graph &g;
        STORED connected;
        INITIALS initials;

        NodeBuilder(Graph &g, STORED connected = {}, INITIALS initials = {})
            : g(g), connected(connected), initials(initials) {}

        friend class Graph;
        template <template <typename> class,
                  template <template <typename> class, typename> class,
                  class...>
        friend class NodeBuilder;

        template <template <class> class POLICY, typename VAL,
                  typename input_type = typename POLICY<VAL>::input_type>
        inline auto doconnect(Connectable<input_type> *arg,
                              input_type &&initial = {}) {

            auto new_arg =
                std::make_tuple<Connectable<input_type> *>(std::move(arg));
            auto new_initial = std::make_tuple<input_type>(std::move(initial));

            auto new_connected = std::tuple_cat(connected, std::move(new_arg));
            auto new_initials =
                std::tuple_cat(initials, std::move(new_initial));

            return NodeBuilder<PROPAGATE, OUTPUT, INPUTS..., POLICY<VAL>>(
                g, new_connected, new_initials);
        }
    };

    // see comments in declaration
    NodeBuilder<Always, SingleList> Graph::node() {
        return NodeBuilder<Always, SingleList>(*this);
    }

    // see comments in declaration
    void WorkState::add_to_queue(Work &work) {
        // note that the or-equals part of the check is important; if we
        // failed
        // to calculate work this time then work.id == current_id, and we
        // want to put the work back on the graph queue for later
        // evaluation.
        if (work.id <= current_id) {
            // process it next Graph()
            work.schedule(g);

            if (stats)
                stats->pushed_graph++;
        } else {
            // keep anything around that's going on the heap - we remove a
            // reference after popping them off the heap and eval()'ing them
            intrusive_ptr_add_ref(&work);

            q.push(&work);

            if (stats)
                stats->pushed_heap++;
        }
    }

    // see comments in declaration
    constexpr bool WorkQueueCmp::operator()(const Work *a,
                                            const Work *b) const {
        return a->id > b->id;
    }

    // see comments in declaration
    bool Graph::operator()(struct Stats *stats) {
        if (stats)
            *stats = EmptyStats;

        auto head = work_queue.exchange(&tombstone, std::memory_order_acq_rel);
        if (head == &tombstone)
            return false;

        WorkState work(*this, stats);
        Work *w = head;
        while (w != &tombstone) {
            // remove us from the work queue. Note that this is slightly
            // inefficient, as w could be put back on the Graph's work_queue
            // before it's been evaluated in this function call, and so is
            // needlessly evaluated a second time.
            Work *next = w->dequeue();

            work.q.push(w);
            if (stats)
                stats->queued++;

            w = next;
        }

        while (!work.q.empty()) {

            Work *w = work.q.top();
            work.q.pop();

            // remove any duplicates, we only need to
            // calculate things once.
            while (!work.q.empty() && work.q.top()->id == w->id) {
                intrusive_ptr_release(work.q.top());
                work.q.pop();
                if (stats)
                    stats->duplicates++;
            }

            work.current_id = w->id;
            w->eval(work);
            if (stats)
                stats->worked++;

            // finally finished with this Work - it's not on the Graph queue
            // or the heap
            intrusive_ptr_release(w);
        }

        return true;
    }

    // see comments in declaration
    void Work::schedule(Graph &g) {
        if (id == flags::DONT_SCHEDULE)
            return;

        // don't want work to be deleted while queued
        intrusive_ptr_add_ref(this);

        bool first_time = true;
        while (true) {
            std::uintptr_t current = next.load(std::memory_order_acquire);
            bool locked = current & flags::LOCK;

            if (first_time && (current & ~flags::LOCK)) {
                // we're already on the work queue, as we're pointing to a
                // non-zero pointer
                intrusive_ptr_release(this);
                return;
            }

            // add w to the queue by chaning its next pointer to point
            // to the head of the queue
            Work *head = g.work_queue.load(std::memory_order_acquire);
            if (!next.compare_exchange_weak(
                    current, reinterpret_cast<std::uintptr_t>(head) | locked)) {
                // next was updated under us, retry from the start
                continue;
            }

            if (g.work_queue.compare_exchange_weak(head, this)) {
                // success! but keep the intrustive reference active
                return;
            }

            // if we're here we pointed w.next to the head of the queue,
            // but something changed the queue before we could finish. The
            // next time round the loop we know current will not be nullptr,
            // so set a flag to skip the are-we-already-queued check.
            first_time = false;
        }
    }

    /**
     * @brief Repeatedly evaluate the Graph's work queue
     * @details Evaluate in a busy-loop, only yielding when there's no work
     * to do. Doesn't block. This avoids having to pay the cost of mutex-related
     * system calls if we slept on a mutex (and appending an Input would
     * have to lock the same mutex when notifying this evalution thread).
     *
     * @param g The graph containing the work_queue to evaluate
     * @param stop When set, the thread will exit its busy-loop the next
     * time it sees the work_queue empty
     */
    void evaluate_repeatedly(Graph &g, std::atomic<bool> &stop) {
        while (!stop.load(std::memory_order_consume)) {
            while (g()) {
            }
            std::this_thread::yield();
        }
    }
}

#endif