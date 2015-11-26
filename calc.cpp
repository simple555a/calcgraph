#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <forward_list>
#include <memory>
#include <queue>
#include <thread>
#include <tuple>

#include "calc.h"

namespace calc {
    class Graph;
    class Work;
    template<typename OUT>
    class Evaluation;

    void WorkState::add_to_queue(Work& work) {
        if (work.id <= current_id)
            // process it next Graph#eval
            g.add_to_queue(work);
        else
            // spin-lock it and add to our queue
            q.push(&work);
    }

    constexpr bool WorkQueueCmp::operator()(const Work* a, const Work* b) {
        return b->id > a->id;
    }

    void WorkState::operator()() {
        Work* w = q.top();
        q.pop();
        current_id = w->id;
        w->eval(*this);

        // finally finished with this Work - it's not on the Graph queue or the heap
        intrusive_ptr_release(w);
    }

    /**
     * Doesn't release the locks we have on the Work* items in the queue, as we'll
     * just put them in a heap.
     */
    void Graph::operator()() {
        auto head = this->next.exchange(this);
        if (head == this)
            return;

        auto work = WorkState(*this);
        for (auto w = head; w != this; w = w->next.load()) {
            work.q.push(w);
        }

        while (!work.q.empty()) {
            work();
        }
    }

    void Graph::add_to_queue(Work& w) {
        // don't want work to be deleted while queued
        intrusive_ptr_add_ref(&w);

        Work* snap;
        while (true) {

            // only queue us up if we're unlocked or locked, i.e.
            // not if we're already on the work queue.
            auto lock = w.next.load();
            if (lock != nullptr && lock != &w) {
                intrusive_ptr_release(&w);
                return;
            }

            // add w to the queue by chaning its `next` pointer to point
            // to the head of the queue
            snap = this->next.load();
            if (!w.next.compare_exchange_weak(lock, snap))
                continue;

            if (this->next.compare_exchange_weak(snap, &w))
                // success! but keep the intrustive reference active
                return;

            // if we're here we pointed `w.next` to the head of the queue,
            // but something changed the queue before we could update it
            // to `w`. Undo (i.e. unlock) `w`:
            w.next.store(lock);
        }
    }
}

