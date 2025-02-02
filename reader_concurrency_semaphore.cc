/*
 * Copyright (C) 2018-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <seastar/core/seastar.hh>
#include <seastar/core/print.hh>
#include <seastar/core/file.hh>
#include <seastar/util/lazy.hh>
#include <seastar/util/log.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>

#include "reader_concurrency_semaphore.hh"
#include "utils/exceptions.hh"
#include "schema.hh"
#include "utils/human_readable.hh"

logger rcslog("reader_concurrency_semaphore");

std::ostream& operator<<(std::ostream& os , const reader_resources& r) {
    os << "{" << r.count << ", " << r.memory << "}";
    return os;
}

reader_permit::resource_units::resource_units(reader_permit permit, reader_resources res, already_consumed_tag)
    : _permit(std::move(permit)), _resources(res) {
}

reader_permit::resource_units::resource_units(reader_permit permit, reader_resources res)
    : _permit(std::move(permit)) {
    _permit.consume(res);
    _resources = res;
}

reader_permit::resource_units::resource_units(resource_units&& o) noexcept
    : _permit(std::move(o._permit))
    , _resources(std::exchange(o._resources, {})) {
}

reader_permit::resource_units::~resource_units() {
    if (_resources.non_zero()) {
        reset();
    }
}

reader_permit::resource_units& reader_permit::resource_units::operator=(resource_units&& o) noexcept {
    if (&o == this) {
        return *this;
    }
    reset();
    _permit = std::move(o._permit);
    _resources = std::exchange(o._resources, {});
    return *this;
}

void reader_permit::resource_units::add(resource_units&& o) {
    assert(_permit == o._permit);
    _resources += std::exchange(o._resources, {});
}

void reader_permit::resource_units::reset(reader_resources res) {
    if (res.non_zero()) {
        _permit.consume(res);
    }
    if (_resources.non_zero()) {
        _permit.signal(_resources);
    }
    _resources = res;
}

class reader_permit::impl
        : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>
        , public enable_shared_from_this<reader_permit::impl> {
    reader_concurrency_semaphore& _semaphore;
    const schema* _schema;
    sstring _op_name;
    std::string_view _op_name_view;
    reader_resources _base_resources;
    bool _base_resources_consumed = false;
    reader_resources _resources;
    reader_permit::state _state = reader_permit::state::active_unused;
    uint64_t _used_branches = 0;
    bool _marked_as_used = false;
    uint64_t _blocked_branches = 0;
    bool _marked_as_blocked = false;
    db::timeout_clock::time_point _timeout;
    query::max_result_size _max_result_size{query::result_memory_limiter::unlimited_result_size};
    uint64_t _sstables_read = 0;
    size_t _requested_memory = 0;
    std::optional<shared_future<>> _memory_future;
    uint64_t _oom_kills = 0;

private:
    void on_permit_used() {
        _semaphore.on_permit_used();
        _marked_as_used = true;
    }
    void on_permit_unused() {
        _semaphore.on_permit_unused();
        _marked_as_used = false;
    }
    void on_permit_blocked() {
        _semaphore.on_permit_blocked();
        _marked_as_blocked = true;
    }
    void on_permit_unblocked() {
        _semaphore.on_permit_unblocked();
        _marked_as_blocked = false;
    }
    void on_permit_active() {
        if (_used_branches) {
            _state = reader_permit::state::active_used;
            on_permit_used();
            if (_blocked_branches) {
                _state = reader_permit::state::active_blocked;
                on_permit_blocked();
            }
        } else {
            _state = reader_permit::state::active_unused;
        }
    }

    void on_permit_inactive(reader_permit::state st) {
        _state = st;
        if (_marked_as_blocked) {
            on_permit_unblocked();
        }
        if (_marked_as_used) {
            on_permit_unused();
        }
    }

public:
    struct value_tag {};

    impl(reader_concurrency_semaphore& semaphore, const schema* const schema, const std::string_view& op_name, reader_resources base_resources, db::timeout_clock::time_point timeout)
        : _semaphore(semaphore)
        , _schema(schema)
        , _op_name_view(op_name)
        , _base_resources(base_resources)
        , _timeout(timeout)
    {
        _semaphore.on_permit_created(*this);
    }
    impl(reader_concurrency_semaphore& semaphore, const schema* const schema, sstring&& op_name, reader_resources base_resources, db::timeout_clock::time_point timeout)
        : _semaphore(semaphore)
        , _schema(schema)
        , _op_name(std::move(op_name))
        , _op_name_view(_op_name)
        , _base_resources(base_resources)
        , _timeout(timeout)
    {
        _semaphore.on_permit_created(*this);
    }
    ~impl() {
        if (_base_resources_consumed) {
            signal(_base_resources);
        }

        if (_resources.non_zero()) {
            on_internal_error_noexcept(rcslog, format("reader_permit::impl::~impl(): permit {} detected a leak of {{count={}, memory={}}} resources",
                        description(),
                        _resources.count,
                        _resources.memory));
            signal(_resources);
        }

        if (_used_branches) {
            on_internal_error_noexcept(rcslog, format("reader_permit::impl::~impl(): permit {}.{}:{} destroyed with {} used branches",
                        _schema ? _schema->ks_name() : "*",
                        _schema ? _schema->cf_name() : "*",
                        _op_name_view,
                        _used_branches));
            _semaphore.on_permit_unused();
        }

        if (_blocked_branches) {
            on_internal_error_noexcept(rcslog, format("reader_permit::impl::~impl(): permit {}.{}:{} destroyed with {} blocked branches",
                        _schema ? _schema->ks_name() : "*",
                        _schema ? _schema->cf_name() : "*",
                        _op_name_view,
                        _blocked_branches));
            _semaphore.on_permit_unblocked();
        }

        // Should probably make a scene here, but its not worth it.
        _semaphore._stats.sstables_read -= _sstables_read;
        _semaphore._stats.disk_reads -= bool(_sstables_read);

        _semaphore.on_permit_destroyed(*this);
    }

    reader_concurrency_semaphore& semaphore() {
        return _semaphore;
    }

    const ::schema* get_schema() const {
        return _schema;
    }

    std::string_view get_op_name() const {
        return _op_name_view;
    }

    reader_permit::state get_state() const {
        return _state;
    }

    void on_waiting_for_admission() {
        on_permit_inactive(reader_permit::state::waiting_for_admission);
    }

    void on_waiting_for_memory(future<> fut) {
        on_permit_inactive(reader_permit::state::waiting_for_memory);
        _memory_future.emplace(std::move(fut));
    }

    void on_admission() {
        assert(_state != reader_permit::state::active_blocked);
        on_permit_active();
        consume(_base_resources);
        _base_resources_consumed = true;
    }

    void on_granted_memory() {
        if (_state == reader_permit::state::waiting_for_memory) {
            on_permit_active();
        }
        consume({0, std::exchange(_requested_memory, 0)});
    }

    future<> get_memory_future() {
        return _memory_future->get_future();
    }

    void on_register_as_inactive() {
        assert(_state == reader_permit::state::active_unused || _state == reader_permit::state::active_used);
        on_permit_inactive(reader_permit::state::inactive);
    }

    void on_unregister_as_inactive() {
        assert(_state == reader_permit::state::inactive);
        on_permit_active();
    }

    void on_evicted() {
        assert(_state == reader_permit::state::inactive);
        _state = reader_permit::state::evicted;
        if (_base_resources_consumed) {
            signal(_base_resources);
            _base_resources_consumed = false;
        }
    }

    void consume(reader_resources res) {
        _semaphore.consume(*this, res);
        _resources += res;
    }

    void signal(reader_resources res) {
        _resources -= res;
        _semaphore.signal(res);
    }

    future<resource_units> request_memory(size_t memory) {
        _requested_memory += memory;
        return _semaphore.request_memory(*this, memory).then([this, memory] {
            return resource_units(reader_permit(shared_from_this()), {0, ssize_t(memory)}, resource_units::already_consumed_tag{});
        });
    }

    reader_resources resources() const {
        return _resources;
    }

    reader_resources base_resources() const {
        return _base_resources;
    }

    void release_base_resources() noexcept {
        if (_base_resources_consumed) {
            _resources -= _base_resources;
            _base_resources_consumed = false;
        }
        _semaphore.signal(std::exchange(_base_resources, {}));
    }

    sstring description() const {
        return format("{}.{}:{}",
                _schema ? _schema->ks_name() : "*",
                _schema ? _schema->cf_name() : "*",
                _op_name_view);
    }

    void mark_used() noexcept {
        ++_used_branches;
        if (!_marked_as_used && _state == reader_permit::state::active_unused) {
            _state = reader_permit::state::active_used;
            on_permit_used();
            if (_blocked_branches && !_marked_as_blocked) {
                _state = reader_permit::state::active_blocked;
                on_permit_blocked();
            }
        }
    }

    void mark_unused() noexcept {
        assert(_used_branches);
        --_used_branches;
        if (_marked_as_used && !_used_branches) {
            // When an exception is thrown, blocked and used guards might be
            // destroyed out-of-order. Force an unblock here so that we maintain
            // used >= blocked.
            if (_marked_as_blocked) {
                on_permit_unblocked();
            }
            _state = reader_permit::state::active_unused;
            on_permit_unused();
        }
    }

    void mark_blocked() noexcept {
        ++_blocked_branches;
        if (_blocked_branches == 1 && _state == reader_permit::state::active_used) {
            _state = reader_permit::state::active_blocked;
            on_permit_blocked();
        }
    }

    void mark_unblocked() noexcept {
        assert(_blocked_branches);
        --_blocked_branches;
        if (_marked_as_blocked && !_blocked_branches) {
            _state = reader_permit::state::active_used;
            on_permit_unblocked();
        }
    }

    bool needs_readmission() const {
        return _state == reader_permit::state::evicted;
    }

    future<> wait_readmission() {
        return _semaphore.do_wait_admission(shared_from_this());
    }

    db::timeout_clock::time_point timeout() const noexcept {
        return _timeout;
    }

    void set_timeout(db::timeout_clock::time_point timeout) noexcept {
        using namespace std::chrono_literals;
        if (_timeout != db::no_timeout && timeout < _timeout) {
            if (_timeout - timeout > 100ms) {
                rcslog.warn("Detected timeout skew of {}ms, please check time skew between nodes in the cluster.  backtrace: {}",
                        std::chrono::duration_cast<std::chrono::milliseconds>(_timeout - timeout).count(),
                        current_backtrace());
            }
        }
        _timeout = timeout;
    }

    query::max_result_size max_result_size() const {
        return _max_result_size;
    }

    void set_max_result_size(query::max_result_size s) {
        _max_result_size = std::move(s);
    }

    void on_start_sstable_read() noexcept {
        if (!_sstables_read) {
            ++_semaphore._stats.disk_reads;
        }
        ++_sstables_read;
        ++_semaphore._stats.sstables_read;
    }

    void on_finish_sstable_read() noexcept {
        --_sstables_read;
        --_semaphore._stats.sstables_read;
        if (!_sstables_read) {
            --_semaphore._stats.disk_reads;
        }
    }

    bool on_oom_kill() noexcept {
        return !bool(_oom_kills++);
    }
};

static_assert(std::is_nothrow_copy_constructible_v<reader_permit>);
static_assert(std::is_nothrow_move_constructible_v<reader_permit>);

reader_permit::reader_permit(shared_ptr<impl> impl) : _impl(std::move(impl))
{
}

reader_permit::reader_permit(reader_concurrency_semaphore& semaphore, const schema* const schema, std::string_view op_name,
        reader_resources base_resources, db::timeout_clock::time_point timeout)
    : _impl(::seastar::make_shared<reader_permit::impl>(semaphore, schema, op_name, base_resources, timeout))
{
}

reader_permit::reader_permit(reader_concurrency_semaphore& semaphore, const schema* const schema, sstring&& op_name,
        reader_resources base_resources, db::timeout_clock::time_point timeout)
    : _impl(::seastar::make_shared<reader_permit::impl>(semaphore, schema, std::move(op_name), base_resources, timeout))
{
}

void reader_permit::on_waiting_for_admission() {
    _impl->on_waiting_for_admission();
}

void reader_permit::on_waiting_for_memory(future<> fut) {
    _impl->on_waiting_for_memory(std::move(fut));
}

void reader_permit::on_admission() {
    _impl->on_admission();
}

void reader_permit::on_granted_memory() {
    _impl->on_granted_memory();
}

future<> reader_permit::get_memory_future() {
    return _impl->get_memory_future();
}

reader_permit::~reader_permit() {
}

reader_concurrency_semaphore& reader_permit::semaphore() {
    return _impl->semaphore();
}

reader_permit::state reader_permit::get_state() const {
    return _impl->get_state();
}

bool reader_permit::needs_readmission() const {
    return _impl->needs_readmission();
}

future<> reader_permit::wait_readmission() {
    return _impl->wait_readmission();
}

void reader_permit::consume(reader_resources res) {
    _impl->consume(res);
}

void reader_permit::signal(reader_resources res) {
    _impl->signal(res);
}

reader_permit::resource_units reader_permit::consume_memory(size_t memory) {
    return consume_resources(reader_resources{0, ssize_t(memory)});
}

reader_permit::resource_units reader_permit::consume_resources(reader_resources res) {
    return resource_units(*this, res);
}

future<reader_permit::resource_units> reader_permit::request_memory(size_t memory) {
    return _impl->request_memory(memory);
}

reader_resources reader_permit::consumed_resources() const {
    return _impl->resources();
}

reader_resources reader_permit::base_resources() const {
    return _impl->base_resources();
}

void reader_permit::release_base_resources() noexcept {
    return _impl->release_base_resources();
}

sstring reader_permit::description() const {
    return _impl->description();
}

void reader_permit::mark_used() noexcept {
    _impl->mark_used();
}

void reader_permit::mark_unused() noexcept {
    _impl->mark_unused();
}

void reader_permit::mark_blocked() noexcept {
    _impl->mark_blocked();
}

void reader_permit::mark_unblocked() noexcept {
    _impl->mark_unblocked();
}

db::timeout_clock::time_point reader_permit::timeout() const noexcept {
    return _impl->timeout();
}

void reader_permit::set_timeout(db::timeout_clock::time_point timeout) noexcept {
    _impl->set_timeout(timeout);
}

query::max_result_size reader_permit::max_result_size() const {
    return _impl->max_result_size();
}

void reader_permit::set_max_result_size(query::max_result_size s) {
    _impl->set_max_result_size(std::move(s));
}

void reader_permit::on_start_sstable_read() noexcept {
    _impl->on_start_sstable_read();
}

void reader_permit::on_finish_sstable_read() noexcept {
    _impl->on_finish_sstable_read();
}

std::ostream& operator<<(std::ostream& os, reader_permit::state s) {
    switch (s) {
        case reader_permit::state::waiting_for_admission:
            os << "waiting_for_admission";
            break;
        case reader_permit::state::waiting_for_memory:
            os << "waiting_for_memory";
            break;
        case reader_permit::state::active_unused:
            os << "active/unused";
            break;
        case reader_permit::state::active_used:
            os << "active/used";
            break;
        case reader_permit::state::active_blocked:
            os << "active/blocked";
            break;
        case reader_permit::state::inactive:
            os << "inactive";
            break;
        case reader_permit::state::evicted:
            os << "evicted";
            break;
    }
    return os;
}

namespace {

struct permit_stats {
    uint64_t permits = 0;
    reader_resources resources;

    void add(const reader_permit::impl& permit) {
        ++permits;
        resources += permit.resources();
    }

    permit_stats& operator+=(const permit_stats& o) {
        permits += o.permits;
        resources += o.resources;
        return *this;
    }
};

using permit_group_key = std::tuple<const schema*, std::string_view, reader_permit::state>;

struct permit_group_key_hash {
    size_t operator()(const permit_group_key& k) const {
        using underlying_type = std::underlying_type_t<reader_permit::state>;
        return std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(std::get<0>(k)))
            ^ std::hash<std::string_view>()(std::get<1>(k))
            ^ std::hash<underlying_type>()(static_cast<underlying_type>(std::get<2>(k)));
    }
};

using permit_groups = std::unordered_map<permit_group_key, permit_stats, permit_group_key_hash>;

static permit_stats do_dump_reader_permit_diagnostics(std::ostream& os, const permit_groups& permits, unsigned max_lines = 20) {
    struct permit_summary {
        const schema* s;
        std::string_view op_name;
        reader_permit::state state;
        uint64_t permits;
        reader_resources resources;
    };

    std::vector<permit_summary> permit_summaries;
    for (const auto& [k, v] : permits) {
        const auto& [s, op_name, k_state] = k;
        permit_summaries.emplace_back(permit_summary{s, op_name, k_state, v.permits, v.resources});
    }

    std::ranges::sort(permit_summaries, [] (const permit_summary& a, const permit_summary& b) {
        return a.resources.memory > b.resources.memory;
    });

    permit_stats total;
    unsigned lines = 0;
    permit_stats omitted_permit_stats;

    auto print_line = [&os] (auto col1, auto col2, auto col3, auto col4) {
        fmt::print(os, "{}\t{}\t{}\t{}\n", col1, col2, col3, col4);
    };

    print_line("permits", "count", "memory", "table/description/state");
    for (const auto& summary : permit_summaries) {
        total.permits += summary.permits;
        total.resources += summary.resources;
        if (!max_lines || lines++ < max_lines) {
            print_line(summary.permits, summary.resources.count, utils::to_hr_size(summary.resources.memory), fmt::format("{}.{}/{}/{}",
                        summary.s ? summary.s->ks_name() : "*",
                        summary.s ? summary.s->cf_name() : "*",
                        summary.op_name,
                        summary.state));
        } else {
            omitted_permit_stats.permits += summary.permits;
            omitted_permit_stats.resources += summary.resources;
        }
    }
    if (max_lines && lines > max_lines) {
        print_line(omitted_permit_stats.permits, omitted_permit_stats.resources.count, utils::to_hr_size(omitted_permit_stats.resources.memory), "permits omitted for brevity");
    }
    fmt::print(os, "\n");
    print_line(total.permits, total.resources.count, utils::to_hr_size(total.resources.memory), "total");
    return total;
}

static void do_dump_reader_permit_diagnostics(std::ostream& os, const reader_concurrency_semaphore& semaphore,
        const reader_concurrency_semaphore::permit_list_type& list, std::string_view problem, unsigned max_lines = 20) {
    permit_groups permits;

    for (const auto& permit : list) {
        permits[permit_group_key(permit.get_schema(), permit.get_op_name(), permit.get_state())].add(permit);
    }

    permit_stats total;

    fmt::print(os, "Semaphore {} with {}/{} count and {}/{} memory resources: {}, dumping permit diagnostics:\n",
            semaphore.name(),
            semaphore.initial_resources().count - semaphore.available_resources().count,
            semaphore.initial_resources().count,
            semaphore.initial_resources().memory - semaphore.available_resources().memory,
            semaphore.initial_resources().memory,
            problem);
    total += do_dump_reader_permit_diagnostics(os, permits, max_lines);
    fmt::print(os, "\n");
    fmt::print(os, "Total: {} permits with {} count and {} memory resources\n", total.permits, total.resources.count, utils::to_hr_size(total.resources.memory));
}

static void maybe_dump_reader_permit_diagnostics(const reader_concurrency_semaphore& semaphore, const reader_concurrency_semaphore::permit_list_type& list,
        std::string_view problem) {
    static thread_local logger::rate_limit rate_limit(std::chrono::seconds(30));

    rcslog.log(log_level::info, rate_limit, "{}", value_of([&] {
        std::ostringstream os;
        do_dump_reader_permit_diagnostics(os, semaphore, list, problem);
        return os.str();
    }));
}

} // anonymous namespace

void reader_concurrency_semaphore::expiry_handler::operator()(entry& e) noexcept {
    e.pr.set_exception(named_semaphore_timed_out(_semaphore._name));

    maybe_dump_reader_permit_diagnostics(_semaphore, _semaphore._permit_list, "timed out");
}

reader_concurrency_semaphore::inactive_read::~inactive_read() {
    detach();
}

void reader_concurrency_semaphore::inactive_read::detach() noexcept {
    if (handle) {
        handle->_irp = nullptr;
        handle = nullptr;
    }
}

void reader_concurrency_semaphore::inactive_read_handle::abandon() noexcept {
    if (_irp) {
        _sem->close_reader(std::move(_irp->reader));
        delete std::exchange(_irp, nullptr);
    }
}

namespace {

struct stop_execution_loop {
};

}

future<> reader_concurrency_semaphore::execution_loop() noexcept {
    while (!_stopped) {
        try {
            co_await _ready_list.not_empty();
        } catch (stop_execution_loop) {
            co_return;
        }

        while (!_ready_list.empty()) {
            auto e = _ready_list.pop();

            try {
                e.func(std::move(e.permit)).forward_to(std::move(e.pr));
            } catch (...) {
                e.pr.set_exception(std::current_exception());
            }

            if (need_preempt()) {
                co_await coroutine::maybe_yield();
            }
        }
    }
}

uint64_t reader_concurrency_semaphore::get_serialize_limit() const {
    if (!_serialize_limit_multiplier() || _serialize_limit_multiplier() == std::numeric_limits<uint32_t>::max() || is_unlimited()) [[unlikely]] {
        return std::numeric_limits<uint64_t>::max();
    }
    return _initial_resources.memory * _serialize_limit_multiplier();
}

uint64_t reader_concurrency_semaphore::get_kill_limit() const {
    if (!_kill_limit_multiplier() || _kill_limit_multiplier() == std::numeric_limits<uint32_t>::max() || is_unlimited()) [[unlikely]] {
        return std::numeric_limits<uint64_t>::max();
    }
    return _initial_resources.memory * _kill_limit_multiplier();
}

void reader_concurrency_semaphore::consume(reader_permit::impl& permit, resources r) {
    // We check whether we even reached the memory limit first.
    // This is a cheap check and should be false most of the time, providing a
    // cheap short-circuit.
    if (_resources.memory <= 0 && (consumed_resources().memory + r.memory) >= get_kill_limit()) [[unlikely]] {
        if (permit.on_oom_kill()) {
            ++_stats.total_reads_killed_due_to_kill_limit;
        }
        maybe_dump_reader_permit_diagnostics(*this, _permit_list, "kill limit triggered");
        throw std::bad_alloc();
    }
    _resources -= r;
}

void reader_concurrency_semaphore::signal(const resources& r) noexcept {
    _resources += r;
    maybe_admit_waiters();
}

reader_concurrency_semaphore::reader_concurrency_semaphore(int count, ssize_t memory, sstring name, size_t max_queue_length,
            utils::updateable_value<uint32_t> serialize_limit_multiplier, utils::updateable_value<uint32_t> kill_limit_multiplier)
    : _initial_resources(count, memory)
    , _resources(count, memory)
    , _wait_list(expiry_handler(*this))
    , _ready_list(max_queue_length)
    , _name(std::move(name))
    , _max_queue_length(max_queue_length)
    , _serialize_limit_multiplier(std::move(serialize_limit_multiplier))
    , _kill_limit_multiplier(std::move(kill_limit_multiplier))
{ }

reader_concurrency_semaphore::reader_concurrency_semaphore(no_limits, sstring name)
    : reader_concurrency_semaphore(
            std::numeric_limits<int>::max(),
            std::numeric_limits<ssize_t>::max(),
            std::move(name),
            std::numeric_limits<size_t>::max(),
            utils::updateable_value(std::numeric_limits<uint32_t>::max()),
            utils::updateable_value(std::numeric_limits<uint32_t>::max())) {}

reader_concurrency_semaphore::~reader_concurrency_semaphore() {
    if (!_stats.total_permits) {
        // We allow destroy without stop() when the semaphore wasn't used at all yet.
        return;
    }
    if (!_stopped) {
        on_internal_error_noexcept(rcslog, format("~reader_concurrency_semaphore(): semaphore {} not stopped before destruction", _name));
        // With the below conditions, we can get away with the semaphore being
        // unstopped. In this case don't force an abort.
        assert(_inactive_reads.empty() && !_close_readers_gate.get_count() && !_permit_gate.get_count() && !_execution_loop_future);
        broken();
    }
}

reader_concurrency_semaphore::inactive_read_handle reader_concurrency_semaphore::register_inactive_read(flat_mutation_reader_v2 reader) noexcept {
    auto& permit_impl = *reader.permit()._impl;
    permit_impl.on_register_as_inactive();
    // Implies _inactive_reads.empty(), we don't queue new readers before
    // evicting all inactive reads.
    // Checking the _wait_list covers the count resources only, so check memory
    // separately.
    if (_wait_list.empty() && _resources.memory > 0) {
      try {
        auto irp = std::make_unique<inactive_read>(std::move(reader));
        auto& ir = *irp;
        _inactive_reads.push_back(ir);
        ++_stats.inactive_reads;
        return inactive_read_handle(*this, *irp.release());
      } catch (...) {
        // It is okay to swallow the exception since
        // we're allowed to drop the reader upon registration
        // due to lack of resources. Returning an empty
        // i_r_h here rather than throwing simplifies the caller's
        // error handling.
        rcslog.warn("Registering inactive read failed: {}. Ignored as if it was evicted.", std::current_exception());
      }
    } else {
        permit_impl.on_evicted();
        ++_stats.permit_based_evictions;
    }
    close_reader(std::move(reader));
    return inactive_read_handle();
}

void reader_concurrency_semaphore::set_notify_handler(inactive_read_handle& irh, eviction_notify_handler&& notify_handler, std::optional<std::chrono::seconds> ttl_opt) {
    auto& ir = *irh._irp;
    ir.notify_handler = std::move(notify_handler);
    if (ttl_opt) {
        ir.ttl_timer.set_callback([this, &ir] {
            evict(ir, evict_reason::time);
        });
        ir.ttl_timer.arm(lowres_clock::now() + *ttl_opt);
    }
}

flat_mutation_reader_v2_opt reader_concurrency_semaphore::unregister_inactive_read(inactive_read_handle irh) {
    if (!irh) {
        return {};
    }
    if (irh._sem != this) {
        // unregister from the other semaphore
        // and close the reader, in case on_internal_error
        // doesn't abort.
        auto irp = std::move(irh._irp);
        irp->unlink();
        irh._sem->close_reader(std::move(irp->reader));
        on_internal_error(rcslog, fmt::format(
                    "reader_concurrency_semaphore::unregister_inactive_read(): "
                    "attempted to unregister an inactive read with a handle belonging to another semaphore: "
                    "this is {} (0x{:x}) but the handle belongs to {} (0x{:x})",
                    name(),
                    reinterpret_cast<uintptr_t>(this),
                    irh._sem->name(),
                    reinterpret_cast<uintptr_t>(irh._sem)));
    }

    --_stats.inactive_reads;
    std::unique_ptr<inactive_read> irp(irh._irp);
    irp->reader.permit()._impl->on_unregister_as_inactive();
    return std::move(irp->reader);
}

bool reader_concurrency_semaphore::try_evict_one_inactive_read(evict_reason reason) {
    if (_inactive_reads.empty()) {
        return false;
    }
    evict(_inactive_reads.front(), reason);
    return true;
}

void reader_concurrency_semaphore::clear_inactive_reads() {
    while (!_inactive_reads.empty()) {
        auto& ir = _inactive_reads.front();
        close_reader(std::move(ir.reader));
        // Destroying the read unlinks it too.
        std::unique_ptr<inactive_read> _(&*_inactive_reads.begin());
    }
}

future<> reader_concurrency_semaphore::evict_inactive_reads_for_table(table_id id) noexcept {
    inactive_reads_type evicted_readers;
    auto it = _inactive_reads.begin();
    while (it != _inactive_reads.end()) {
        auto& ir = *it;
        ++it;
        if (ir.reader.schema()->id() == id) {
            do_detach_inactive_reader(ir, evict_reason::manual);
            evicted_readers.push_back(ir);
        }
    }
    while (!evicted_readers.empty()) {
        std::unique_ptr<inactive_read> irp(&evicted_readers.front());
        co_await irp->reader.close();
    }
}

std::runtime_error reader_concurrency_semaphore::stopped_exception() {
    return std::runtime_error(format("{} was stopped", _name));
}

future<> reader_concurrency_semaphore::stop() noexcept {
    assert(!_stopped);
    _stopped = true;
    co_await stop_ext_pre();
    clear_inactive_reads();
    co_await _close_readers_gate.close();
    co_await _permit_gate.close();
    if (_execution_loop_future) {
        if (_ready_list.has_blocked_consumer()) {
            _ready_list.abort(std::make_exception_ptr(stop_execution_loop{}));
        }
        co_await std::move(*_execution_loop_future);
    }
    broken(std::make_exception_ptr(stopped_exception()));
    co_await stop_ext_post();
    co_return;
}

void reader_concurrency_semaphore::do_detach_inactive_reader(inactive_read& ir, evict_reason reason) noexcept {
    ir.unlink();
    ir.ttl_timer.cancel();
    ir.detach();
    ir.reader.permit()._impl->on_evicted();
    try {
        if (ir.notify_handler) {
            ir.notify_handler(reason);
        }
    } catch (...) {
        rcslog.error("[semaphore {}] evict(): notify handler failed for inactive read evicted due to {}: {}", _name, static_cast<int>(reason), std::current_exception());
    }
    switch (reason) {
        case evict_reason::permit:
            ++_stats.permit_based_evictions;
            break;
        case evict_reason::time:
            ++_stats.time_based_evictions;
            break;
        case evict_reason::manual:
            break;
    }
    --_stats.inactive_reads;
}

flat_mutation_reader_v2 reader_concurrency_semaphore::detach_inactive_reader(inactive_read& ir, evict_reason reason) noexcept {
    std::unique_ptr<inactive_read> irp(&ir);
    do_detach_inactive_reader(ir, reason);
    return std::move(irp->reader);
}

void reader_concurrency_semaphore::evict(inactive_read& ir, evict_reason reason) noexcept {
    close_reader(detach_inactive_reader(ir, reason));
}

void reader_concurrency_semaphore::close_reader(flat_mutation_reader_v2 reader) {
    // It is safe to discard the future since it is waited on indirectly
    // by closing the _close_readers_gate in stop().
    (void)with_gate(_close_readers_gate, [reader = std::move(reader)] () mutable {
        return reader.close();
    });
}

bool reader_concurrency_semaphore::has_available_units(const resources& r) const {
    // Special case: when there is no active reader (based on count) admit one
    // regardless of availability of memory.
    return (_resources.non_zero() && _resources.count >= r.count && _resources.memory >= r.memory) || _resources.count == _initial_resources.count;
}

bool reader_concurrency_semaphore::all_used_permits_are_stalled() const {
    return _stats.used_permits == _stats.blocked_permits;
}

std::exception_ptr reader_concurrency_semaphore::check_queue_size(std::string_view queue_name) {
    if ((_wait_list.size() + _ready_list.size()) >= _max_queue_length) {
        _stats.total_reads_shed_due_to_overload++;
        maybe_dump_reader_permit_diagnostics(*this, _permit_list, fmt::format("{} queue overload", queue_name));
        return std::make_exception_ptr(std::runtime_error(format("{}: {} queue overload", _name, queue_name)));
    }
    return {};
}

future<> reader_concurrency_semaphore::enqueue_waiter(reader_permit permit, read_func func, wait_on wait) {
    if (auto ex = check_queue_size("wait")) {
        return make_exception_future<>(std::move(ex));
    }
    promise<> pr;
    auto fut = pr.get_future();
    auto timeout = permit.timeout();
    if (wait == wait_on::admission) {
        permit.on_waiting_for_admission();
        _wait_list.push_to_admission_queue(entry(std::move(pr), std::move(permit), std::move(func)), timeout);
        ++_stats.reads_enqueued_for_admission;
    } else {
        permit.on_waiting_for_memory(std::move(fut));
        fut = permit.get_memory_future();
        _wait_list.push_to_memory_queue(entry(std::move(pr), std::move(permit), std::move(func)), timeout);
        ++_stats.reads_enqueued_for_memory;
    }
    return fut;
}

void reader_concurrency_semaphore::evict_readers_in_background() {
    if (_evicting) {
        return;
    }
    _evicting = true;
    // Evict inactive readers in the background while wait list isn't empty
    // This is safe since stop() closes _gate;
    (void)with_gate(_close_readers_gate, [this] {
        return repeat([this] {
            if (_wait_list.empty() || _inactive_reads.empty()) {
                _evicting = false;
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            }
            return detach_inactive_reader(_inactive_reads.front(), evict_reason::permit).close().then([] {
                return stop_iteration::no;
            });
        });
    });
}

reader_concurrency_semaphore::can_admit
reader_concurrency_semaphore::can_admit_read(const reader_permit& permit) const noexcept {
    if (_resources.memory < 0) [[unlikely]] {
        const auto consumed_memory = consumed_resources().memory;
        if (consumed_memory >= get_kill_limit()) {
            return can_admit::no;
        }
        if (consumed_memory >= get_serialize_limit()) {
            if (_blessed_permit) {
                // blessed permit is never in the wait list
                return can_admit::no;
            } else {
                auto res = permit.get_state() == reader_permit::state::waiting_for_memory ? can_admit::yes : can_admit::no;
                return res;
            }
        }
    }

    if (permit.get_state() == reader_permit::state::waiting_for_memory) {
        return can_admit::yes;
    }

    if (!_ready_list.empty()) {
        return can_admit::no;
    }

    if (!all_used_permits_are_stalled()) {
        return can_admit::no;
    }

    if (!has_available_units(permit.base_resources())) {
        if (_inactive_reads.empty()) {
            return can_admit::no;
        } else {
            return can_admit::maybe;
        }
    }

    return can_admit::yes;
}

future<> reader_concurrency_semaphore::do_wait_admission(reader_permit permit, read_func func) {
    if (!_execution_loop_future) {
        _execution_loop_future.emplace(execution_loop());
    }

    const auto admit = can_admit_read(permit);
    if (admit != can_admit::yes || !_wait_list.empty()) {
        auto fut = enqueue_waiter(std::move(permit), std::move(func), wait_on::admission);
        if (admit == can_admit::yes && !_wait_list.empty()) {
            // This is a contradiction: the semaphore could admit waiters yet it has waiters.
            // Normally, the semaphore should admit waiters as soon as it can.
            // So at any point in time, there should either be no waiters, or it
            // shouldn't be able to admit new reads. Otherwise something went wrong.
            maybe_dump_reader_permit_diagnostics(*this, _permit_list, "semaphore could admit new reads yet there are waiters");
            maybe_admit_waiters();
        } else if (admit == can_admit::maybe) {
            evict_readers_in_background();
        }
        return fut;
    }

    permit.on_admission();
    ++_stats.reads_admitted;
    if (func) {
        return with_ready_permit(std::move(permit), std::move(func));
    }
    return make_ready_future<>();
}

void reader_concurrency_semaphore::maybe_admit_waiters() noexcept {
    auto admit = can_admit::no;
    while (!_wait_list.empty() && (admit = can_admit_read(_wait_list.front().permit)) == can_admit::yes) {
        auto& x = _wait_list.front();
        try {
            if (x.permit.get_state() == reader_permit::state::waiting_for_memory) {
                _blessed_permit = x.permit._impl.get();
                x.permit.on_granted_memory();
            } else {
                x.permit.on_admission();
                ++_stats.reads_admitted;
            }
            if (x.func) {
                _ready_list.push(std::move(x));
            } else {
                x.pr.set_value();
            }
        } catch (...) {
            x.pr.set_exception(std::current_exception());
        }
        _wait_list.pop_front();
    }
    if (admit == can_admit::maybe) {
        // Evicting readers will trigger another call to `maybe_admit_waiters()` from `signal()`.
        evict_readers_in_background();
    }
}

future<> reader_concurrency_semaphore::request_memory(reader_permit::impl& permit, size_t memory) {
    // Already blocked on memory?
    if (permit.get_state() == reader_permit::state::waiting_for_memory) {
        return permit.get_memory_future();
    }

    if (_resources.memory > 0 || (consumed_resources().memory + memory) < get_serialize_limit()) {
        permit.on_granted_memory();
        return make_ready_future<>();
    }

    if (!_blessed_permit) {
        _blessed_permit = &permit;
    }

    if (_blessed_permit == &permit) {
        permit.on_granted_memory();
        return make_ready_future<>();
    }

    return enqueue_waiter(reader_permit(permit.shared_from_this()), {}, wait_on::memory);
}

void reader_concurrency_semaphore::on_permit_created(reader_permit::impl& permit) {
    _permit_gate.enter();
    _permit_list.push_back(permit);
    ++_stats.total_permits;
    ++_stats.current_permits;
}

void reader_concurrency_semaphore::on_permit_destroyed(reader_permit::impl& permit) noexcept {
    permit.unlink();
    _permit_gate.leave();
    --_stats.current_permits;
    if (_blessed_permit == &permit) {
        _blessed_permit = nullptr;
        maybe_admit_waiters();
    }
}

void reader_concurrency_semaphore::on_permit_used() noexcept {
    ++_stats.used_permits;
}

void reader_concurrency_semaphore::on_permit_unused() noexcept {
    assert(_stats.used_permits);
    --_stats.used_permits;
    assert(_stats.used_permits >= _stats.blocked_permits);
    maybe_admit_waiters();
}

void reader_concurrency_semaphore::on_permit_blocked() noexcept {
    ++_stats.blocked_permits;
    assert(_stats.used_permits >= _stats.blocked_permits);
    maybe_admit_waiters();
}

void reader_concurrency_semaphore::on_permit_unblocked() noexcept {
    assert(_stats.blocked_permits);
    --_stats.blocked_permits;
}

future<reader_permit> reader_concurrency_semaphore::obtain_permit(const schema* const schema, const char* const op_name, size_t memory,
        db::timeout_clock::time_point timeout) {
    auto permit = reader_permit(*this, schema, std::string_view(op_name), {1, static_cast<ssize_t>(memory)}, timeout);
    return do_wait_admission(permit).then([permit] () mutable {
        return std::move(permit);
    });
}

future<reader_permit> reader_concurrency_semaphore::obtain_permit(const schema* const schema, sstring&& op_name, size_t memory,
        db::timeout_clock::time_point timeout) {
    auto permit = reader_permit(*this, schema, std::move(op_name), {1, static_cast<ssize_t>(memory)}, timeout);
    return do_wait_admission(permit).then([permit] () mutable {
        return std::move(permit);
    });
}

reader_permit reader_concurrency_semaphore::make_tracking_only_permit(const schema* const schema, const char* const op_name, db::timeout_clock::time_point timeout) {
    return reader_permit(*this, schema, std::string_view(op_name), {}, timeout);
}

reader_permit reader_concurrency_semaphore::make_tracking_only_permit(const schema* const schema, sstring&& op_name, db::timeout_clock::time_point timeout) {
    return reader_permit(*this, schema, std::move(op_name), {}, timeout);
}

future<> reader_concurrency_semaphore::with_permit(const schema* const schema, const char* const op_name, size_t memory,
        db::timeout_clock::time_point timeout, read_func func) {
    return do_wait_admission(reader_permit(*this, schema, std::string_view(op_name), {1, static_cast<ssize_t>(memory)}, timeout), std::move(func));
}

future<> reader_concurrency_semaphore::with_ready_permit(reader_permit permit, read_func func) {
    if (auto ex = check_queue_size("ready")) {
        return make_exception_future<>(std::move(ex));
    }
    promise<> pr;
    auto fut = pr.get_future();
    _ready_list.push(entry(std::move(pr), std::move(permit), std::move(func)));
    return fut;
}

void reader_concurrency_semaphore::set_resources(resources r) {
    auto delta = r - _initial_resources;
    _initial_resources = r;
    _resources += delta;
    maybe_admit_waiters();
}

void reader_concurrency_semaphore::broken(std::exception_ptr ex) {
    if (!ex) {
        ex = std::make_exception_ptr(broken_semaphore{});
    }
    while (!_wait_list.empty()) {
        _wait_list.front().pr.set_exception(ex);
        _wait_list.pop_front();
    }
}

std::string reader_concurrency_semaphore::dump_diagnostics(unsigned max_lines) const {
    std::ostringstream os;
    do_dump_reader_permit_diagnostics(os, *this, _permit_list, "user request", max_lines);
    return os.str();
}

void reader_concurrency_semaphore::foreach_permit(noncopyable_function<void(const reader_permit&)> func) {
    for (auto& p : _permit_list) {
        func(reader_permit(p.shared_from_this()));
    }
}

// A file that tracks the memory usage of buffers resulting from read
// operations.
class tracking_file_impl : public file_impl {
    file _tracked_file;
    reader_permit _permit;

public:
    tracking_file_impl(file file, reader_permit permit)
        : file_impl(*get_file_impl(file))
        , _tracked_file(std::move(file))
        , _permit(std::move(permit)) {
    }

    tracking_file_impl(const tracking_file_impl&) = delete;
    tracking_file_impl& operator=(const tracking_file_impl&) = delete;
    tracking_file_impl(tracking_file_impl&&) = default;
    tracking_file_impl& operator=(tracking_file_impl&&) = default;

    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->write_dma(pos, buffer, len, pc);
    }

    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->write_dma(pos, std::move(iov), pc);
    }

    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->read_dma(pos, buffer, len, pc);
    }

    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->read_dma(pos, iov, pc);
    }

    virtual future<> flush(void) override {
        return get_file_impl(_tracked_file)->flush();
    }

    virtual future<struct stat> stat(void) override {
        return get_file_impl(_tracked_file)->stat();
    }

    virtual future<> truncate(uint64_t length) override {
        return get_file_impl(_tracked_file)->truncate(length);
    }

    virtual future<> discard(uint64_t offset, uint64_t length) override {
        return get_file_impl(_tracked_file)->discard(offset, length);
    }

    virtual future<> allocate(uint64_t position, uint64_t length) override {
        return get_file_impl(_tracked_file)->allocate(position, length);
    }

    virtual future<uint64_t> size(void) override {
        return get_file_impl(_tracked_file)->size();
    }

    virtual future<> close() override {
        return get_file_impl(_tracked_file)->close();
    }

    virtual std::unique_ptr<file_handle_impl> dup() override {
        return get_file_impl(_tracked_file)->dup();
    }

    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override {
        return get_file_impl(_tracked_file)->list_directory(std::move(next));
    }

    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) override {
        return _permit.request_memory(range_size).then([this, offset, range_size, &pc] (reader_permit::resource_units units) {
            return get_file_impl(_tracked_file)->dma_read_bulk(offset, range_size, pc).then([this, units = std::move(units)] (temporary_buffer<uint8_t> buf) mutable {
                return make_ready_future<temporary_buffer<uint8_t>>(make_tracked_temporary_buffer(std::move(buf), std::move(units)));
            });
        });
    }
};

file make_tracked_file(file f, reader_permit p) {
    return file(make_shared<tracking_file_impl>(f, std::move(p)));
}
