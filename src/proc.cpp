// Utilities for keeping track of jobs, processes and subshells, as well as signal handling
// functions for tracking children. These functions do not themselves launch new processes, the exec
// library will call proc to create representations of the running jobs as needed.
//
// Some of the code in this file is based on code from the Glibc manual.
// IWYU pragma: no_include <__bit_reference>
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <wctype.h>

#include <atomic>
#include <cwchar>

#if HAVE_TERM_H
#include <curses.h>
#include <term.h>
#elif HAVE_NCURSES_TERM_H
#include <ncurses/term.h>
#endif
#include <termios.h>
#ifdef HAVE_SIGINFO_H
#include <siginfo.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/time.h>  // IWYU pragma: keep
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>  // IWYU pragma: keep
#include <memory>
#include <utility>
#include <vector>

#include "common.h"
#include "event.h"
#include "fallback.h"  // IWYU pragma: keep
#include "flog.h"
#include "global_safety.h"
#include "io.h"
#include "job_group.h"
#include "output.h"
#include "parse_tree.h"
#include "parser.h"
#include "proc.h"
#include "reader.h"
#include "sanity.h"
#include "signal.h"
#include "wcstringutil.h"
#include "wutil.h"  // IWYU pragma: keep

/// The signals that signify crashes to us.
static const int crashsignals[] = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGSYS};

static relaxed_atomic_bool_t s_is_interactive_session{false};
bool is_interactive_session() { return s_is_interactive_session; }
void set_interactive_session(bool flag) { s_is_interactive_session = flag; }

static relaxed_atomic_bool_t s_is_login{false};
bool get_login() { return s_is_login; }
void mark_login() { s_is_login = true; }

static relaxed_atomic_bool_t s_no_exec{false};
bool no_exec() { return s_no_exec; }
void mark_no_exec() { s_no_exec = true; }

bool have_proc_stat() {
    // Check for /proc/self/stat to see if we are running with Linux-style procfs.
    static const bool s_result = (access("/proc/self/stat", R_OK) == 0);
    return s_result;
}

static relaxed_atomic_t<job_control_t> job_control_mode{job_control_t::interactive};

job_control_t get_job_control_mode() { return job_control_mode; }

void set_job_control_mode(job_control_t mode) {
    job_control_mode = mode;

    // HACK: when fish (or any shell) launches a job with job control, it will put the job into its
    // own pgroup and call tcsetpgrp() to allow that pgroup to own the terminal (making fish a
    // background process). When the job finishes, fish will try to reclaim the terminal via
    // tcsetpgrp(), but as fish is now a background process it will receive SIGTTOU and stop! Ensure
    // that doesn't happen by ignoring SIGTTOU.
    // Note that if we become interactive, we also ignore SIGTTOU.
    if (mode == job_control_t::all) {
        signal(SIGTTOU, SIG_IGN);
    }
}

void proc_init() { signal_set_handlers_once(false); }

/// Return true if all processes in the job are stopped or completed, and there is at least one
/// stopped process.
bool job_t::is_stopped() const {
    bool has_stopped = false;
    for (const process_ptr_t &p : processes) {
        if (!p->completed && !p->stopped) {
            return false;
        }
        has_stopped |= p->stopped;
    }
    return has_stopped;
}

/// Return true if all processes in the job have completed.
bool job_t::is_completed() const {
    assert(!processes.empty());
    for (const process_ptr_t &p : processes) {
        if (!p->completed) {
            return false;
        }
    }
    return true;
}

bool job_t::posts_job_exit_events() const {
    // If we never got a pgid then we never launched the external process, so don't report it.
    if (!this->get_pgid()) return false;

    // Only report root job exits.
    // For example in `ls | begin ; cat ; end` we don't need to report the cat sub-job.
    if (!flags().is_group_root) return false;

    // Only jobs with external processes post job_exit events.
    return this->has_external_proc();
}

bool job_t::job_chain_is_fully_constructed() const { return group->is_root_constructed(); }

bool job_t::signal(int signal) {
    // Presumably we are distinguishing between the two cases below because we do
    // not want to send ourselves the signal in question in case the job shares
    // a pgid with the shell.
    auto pgid = get_pgid();
    if (pgid.has_value() && *pgid != getpgrp()) {
        if (killpg(*pgid, signal) == -1) {
            char buffer[512];
            sprintf(buffer, "killpg(%d, %s)", *pgid, strsignal(signal));
            wperror(str2wcstring(buffer).c_str());
            return false;
        }
    } else {
        for (const auto &p : processes) {
            if (!p->completed && p->pid && kill(p->pid, signal) == -1) {
                return false;
            }
        }
    }

    return true;
}

maybe_t<statuses_t> job_t::get_statuses() const {
    statuses_t st{};
    bool has_status = false;
    int laststatus = 0;
    st.pipestatus.reserve(processes.size());
    for (const auto &p : processes) {
        auto status = p->status;
        if (status.is_empty()) {
            // Corner case for if a variable assignment is part of a pipeline.
            // e.g. `false | set foo bar | true` will push 1 in the second spot,
            // for a complete pipestatus of `1 1 0`.
            st.pipestatus.push_back(laststatus);
            continue;
        }
        if (status.signal_exited()) {
            st.kill_signal = status.signal_code();
        }
        laststatus = status.status_value();
        has_status = true;
        st.pipestatus.push_back(status.status_value());
    }
    if (!has_status) {
        return none();
    }
    st.status = flags().negate ? !laststatus : laststatus;
    return st;
}

void internal_proc_t::mark_exited(proc_status_t status) {
    assert(!exited() && "Process is already exited");
    status_.store(status, std::memory_order_relaxed);
    exited_.store(true, std::memory_order_release);
    topic_monitor_t::principal().post(topic_t::internal_exit);
    FLOG(proc_internal_proc, L"Internal proc", internal_proc_id_, L"exited with status",
         status.status_value());
}

static int64_t next_proc_id() {
    static std::atomic<uint64_t> s_next{};
    return ++s_next;
}

internal_proc_t::internal_proc_t() : internal_proc_id_(next_proc_id()) {}

job_list_t jobs_requiring_warning_on_exit(const parser_t &parser) {
    job_list_t result;
    for (const auto &job : parser.jobs()) {
        if (!job->is_foreground() && job->is_constructed() && !job->is_completed()) {
            result.push_back(job);
        }
    }
    return result;
}

void print_exit_warning_for_jobs(const job_list_t &jobs) {
    fputws(_(L"There are still jobs active:\n"), stdout);
    fputws(_(L"\n   PID  Command\n"), stdout);
    for (const auto &j : jobs) {
        fwprintf(stdout, L"%6d  %ls\n", j->processes[0]->pid, j->command_wcstr());
    }
    fputws(L"\n", stdout);
    fputws(_(L"A second attempt to exit will terminate them.\n"), stdout);
    fputws(_(L"Use 'disown PID' to remove jobs from the list without terminating them.\n"), stdout);
    reader_schedule_prompt_repaint();
}

/// Set the status of \p proc to \p status.
static void handle_child_status(const shared_ptr<job_t> &job, process_t *proc,
                                proc_status_t status) {
    proc->status = status;
    if (status.stopped()) {
        proc->stopped = true;
    } else if (status.continued()) {
        proc->stopped = false;
    } else {
        proc->completed = true;
    }

    // If the child was killed by SIGINT or SIGQUIT, then treat it as if we received that signal.
    if (status.signal_exited()) {
        int sig = status.signal_code();
        if (sig == SIGINT || sig == SIGQUIT) {
            if (is_interactive_session()) {
                // Mark the job group as cancelled.
                job->group->cancel_with_signal(sig);
            } else {
                // Deliver the SIGINT or SIGQUIT signal to ourself since we're not interactive.
                struct sigaction act;
                sigemptyset(&act.sa_mask);
                act.sa_flags = 0;
                act.sa_handler = SIG_DFL;
                sigaction(sig, &act, nullptr);
                kill(getpid(), sig);
            }
        }
    }
}

process_t::process_t() = default;

void process_t::check_generations_before_launch() {
    gens_ = topic_monitor_t::principal().current_generations();
}

void process_t::mark_aborted_before_launch() {
    this->completed = true;
    // The status may have already been set to e.g. STATUS_NOT_EXECUTABLE.
    // Only stomp a successful status.
    if (this->status.is_success()) {
        this->status = proc_status_t::from_exit_code(EXIT_FAILURE);
    }
}

bool process_t::is_internal() const {
    switch (type) {
        case process_type_t::builtin:
        case process_type_t::function:
        case process_type_t::block_node:
            return true;
        case process_type_t::external:
        case process_type_t::exec:
            return false;
        default:
            assert(false &&
                   "The fish developers forgot to include a process_t. Please report a bug");
            return true;
    }
    assert(false &&
           "process_t::is_internal: Total logic failure, universe is broken. Please replace "
           "universe and retry.");
    return true;
}

wait_handle_ref_t process_t::make_wait_handle(internal_job_id_t jid) {
    if (type != process_type_t::external || pid <= 0) {
        // Not waitable.
        return nullptr;
    }
    if (!wait_handle_) {
        wait_handle_ = std::make_shared<wait_handle_t>(this->pid, jid, wbasename(this->actual_cmd));
    }
    return wait_handle_;
}

static uint64_t next_internal_job_id() {
    static std::atomic<uint64_t> s_next{};
    return ++s_next;
}

job_t::job_t(const properties_t &props, wcstring command_str)
    : properties(props),
      command_str(std::move(command_str)),
      internal_job_id(next_internal_job_id()) {}

job_t::~job_t() = default;

void job_t::mark_constructed() {
    assert(!is_constructed() && "Job was already constructed");
    mut_flags().constructed = true;
    if (flags().is_group_root) {
        group->mark_root_constructed();
    }
}

bool job_t::has_internal_proc() const {
    for (const auto &p : processes) {
        if (p->is_internal()) return true;
    }
    return false;
}

bool job_t::has_external_proc() const {
    for (const auto &p : processes) {
        if (!p->is_internal()) return true;
    }
    return false;
}

/// A list of pids/pgids that have been disowned. They are kept around until either they exit or
/// we exit. Poll these from time-to-time to prevent zombie processes from happening (#5342).
static owning_lock<std::vector<pid_t>> s_disowned_pids;

void add_disowned_job(const job_t *j) {
    assert(j && "Null job");

    // Never add our own (or an invalid) pgid as it is not unique to only
    // one job, and may result in a deadlock if we attempt the wait.
    auto pgid = j->get_pgid();
    auto disowned_pids = s_disowned_pids.acquire();
    if (pgid && *pgid != getpgrp() && *pgid > 0) {
        // waitpid(2) is signalled to wait on a process group rather than a
        // process id by using the negative of its value.
        disowned_pids->push_back(*pgid * -1);
    } else {
        // Instead, add the PIDs of any external processes
        for (auto &process : j->processes) {
            if (process->pid) {
                disowned_pids->push_back(process->pid);
            }
        }
    }
}

// Reap any pids in our disowned list that have exited. This is used to avoid zombies.
static void reap_disowned_pids() {
    auto disowned_pids = s_disowned_pids.acquire();
    auto try_reap1 = [](pid_t pid) {
        int status;
        int ret = waitpid(pid, &status, WNOHANG);
        if (ret > 0) {
            FLOGF(proc_reap_external, "Reaped disowned PID or PGID %d", pid);
        }
        return ret;
    };
    // waitpid returns 0 iff the PID/PGID in question has not changed state; remove the pid/pgid
    // if it has changed or an error occurs (presumably ECHILD because the child does not exist)
    disowned_pids->erase(std::remove_if(disowned_pids->begin(), disowned_pids->end(), try_reap1),
                         disowned_pids->end());
}

/// See if any reapable processes have exited, and mark them accordingly.
/// \param block_ok if no reapable processes have exited, block until one is (or until we receive a
/// signal).
static void process_mark_finished_children(parser_t &parser, bool block_ok) {
    ASSERT_IS_MAIN_THREAD();

    // Get the exit and signal generations of all reapable processes.
    // The exit generation tells us if we have an exit; the signal generation allows for detecting
    // SIGHUP and SIGINT.
    // Go through each process and figure out if and how it wants to be reaped.
    generation_list_t reapgens = generation_list_t::invalids();
    for (const auto &j : parser.jobs()) {
        for (const auto &proc : j->processes) {
            if (!j->can_reap(proc)) continue;

            if (proc->pid > 0) {
                // Reaps with a pid.
                reapgens.set_min_from(topic_t::sigchld, proc->gens_);
                reapgens.set_min_from(topic_t::sighupint, proc->gens_);
            }
            if (proc->internal_proc_) {
                // Reaps with an internal process.
                reapgens.set_min_from(topic_t::internal_exit, proc->gens_);
                reapgens.set_min_from(topic_t::sighupint, proc->gens_);
            }
        }
    }

    // Now check for changes, optionally waiting.
    if (!topic_monitor_t::principal().check(&reapgens, block_ok)) {
        // Nothing changed.
        return;
    }

    // We got some changes. Since we last checked we received SIGCHLD, and or HUP/INT.
    // Update the hup/int generations and reap any reapable processes.
    // We structure this as two loops for some simplicity.
    // First reap all pids.
    for (const auto &j : parser.jobs()) {
        for (const auto &proc : j->processes) {
            // Does this proc have a pid that is reapable?
            if (proc->pid <= 0 || !j->can_reap(proc)) continue;

            // Always update the signal hup/int gen.
            proc->gens_.sighupint = reapgens.sighupint;

            // Nothing to do if we did not get a new sigchld.
            if (proc->gens_.sigchld == reapgens.sigchld) continue;
            proc->gens_.sigchld = reapgens.sigchld;

            // Ok, we are reapable. Run waitpid()!
            int statusv = -1;
            pid_t pid = waitpid(proc->pid, &statusv, WNOHANG | WUNTRACED | WCONTINUED);
            assert((pid <= 0 || pid == proc->pid) && "Unexpcted waitpid() return");
            if (pid <= 0) continue;

            // The process has stopped or exited! Update its status.
            proc_status_t status = proc_status_t::from_waitpid(statusv);
            handle_child_status(j, proc.get(), status);
            if (status.stopped()) {
                j->group->set_is_foreground(false);
            }
            if (status.continued()) {
                j->mut_flags().notified_of_stop = false;
            }
            if (status.normal_exited() || status.signal_exited()) {
                FLOGF(proc_reap_external, "Reaped external process '%ls' (pid %d, status %d)",
                      proc->argv0(), pid, proc->status.status_value());
            } else {
                assert(status.stopped() || status.continued());
                FLOGF(proc_reap_external, "External process '%ls' (pid %d, %s)", proc->argv0(),
                      proc->pid, proc->status.stopped() ? "stopped" : "continued");
            }
        }
    }

    // We are done reaping pids.
    // Reap internal processes.
    for (const auto &j : parser.jobs()) {
        for (const auto &proc : j->processes) {
            // Does this proc have an internal process that is reapable?
            if (!proc->internal_proc_ || !j->can_reap(proc)) continue;

            // Always update the signal hup/int gen.
            proc->gens_.sighupint = reapgens.sighupint;

            // Nothing to do if we did not get a new internal exit.
            if (proc->gens_.internal_exit == reapgens.internal_exit) continue;
            proc->gens_.internal_exit = reapgens.internal_exit;

            // Has the process exited?
            if (!proc->internal_proc_->exited()) continue;

            // The process gets the status from its internal proc.
            handle_child_status(j, proc.get(), proc->internal_proc_->get_status());
            FLOGF(proc_reap_internal, "Reaped internal process '%ls' (id %llu, status %d)",
                  proc->argv0(), proc->internal_proc_->get_id(), proc->status.status_value());
        }
    }

    // Remove any zombies.
    reap_disowned_pids();
}

/// Given a job that has completed, generate job_exit, process_exit, and caller_exit events.
static void generate_exit_events(const job_ref_t &j, const enum_set_t<event_type_t> handled,
                                 std::vector<event_t> *out_evts) {
    // Generate proc and job exit events, except for jobs originating in event handlers.
    if (handled.get(event_type_t::process_exit) && !j->from_event_handler()) {
        for (const auto &proc : j->processes) {
            if (proc->pid > 0) {
                out_evts->push_back(event_t::process_exit(proc->pid, proc->status.status_value()));
            }
        }
    }

    if (handled.get(event_type_t::job_exit) && !j->from_event_handler() &&
        j->posts_job_exit_events()) {
        if (auto last_pid = j->get_last_pid()) {
            out_evts->push_back(event_t::job_exit(*last_pid, j->internal_job_id));
        }
    }

    // Generate caller_exit events.
    if (handled.get(event_type_t::caller_exit)) {
        out_evts->push_back(event_t::caller_exit(j->internal_job_id, j->job_id()));
    }
}

/// \return whether to emit a fish_job_summary call for a process.
static bool proc_wants_summary(const shared_ptr<job_t> &j, const process_ptr_t &p) {
    // Are we completed with a pid?
    if (!p->completed || !p->pid) return false;

    // Did we die due to a signal other than SIGPIPE?
    auto s = p->status;
    if (!s.signal_exited() || s.signal_code() == SIGPIPE) return false;

    // Does the job want to suppress notifications?
    // Note we always report crashes.
    if (j->skip_notification() && !contains(crashsignals, s.signal_code())) return false;

    return true;
}

/// \return whether to emit a fish_job_summary call for a job as a whole. We may also emit this for
/// its individual processes.
static bool job_wants_summary(const shared_ptr<job_t> &j) {
    // Do we just skip notifications?
    if (j->skip_notification()) return false;

    // Do we have a single process which will also report? If so then that suffices for us.
    if (j->processes.size() == 1 && proc_wants_summary(j, j->processes.front())) return false;

    // Are we foreground?
    // The idea here is to not print status messages for jobs that execute in the foreground (i.e.
    // without & and without being `bg`).
    if (j->is_foreground()) return false;

    return true;
}

/// \return whether we want to emit a fish_job_summary call for a job or any of its processes.
bool job_or_proc_wants_summary(const shared_ptr<job_t> &j) {
    if (job_wants_summary(j)) return true;
    for (const auto &p : j->processes) {
        if (proc_wants_summary(j, p)) return true;
    }
    return false;
}

/// Invoke the fish_job_summary function by executing the given command.
static void call_job_summary(parser_t &parser, const wcstring &cmd) {
    event_t event(event_type_t::generic);
    event.desc.str_param1 = L"fish_job_summary";
    block_t *b = parser.push_block(block_t::event_block(event));
    auto saved_status = parser.get_last_statuses();
    parser.eval(cmd, io_chain_t());
    parser.set_last_statuses(saved_status);
    parser.pop_block(b);
}

// \return a command which invokes fish_job_summary.
// The process pointer may be null, in which case it represents the entire job.
// Note this implements the arguments which fish_job_summary expects.
wcstring summary_command(const job_ref_t &j, const process_ptr_t &p = nullptr) {
    wcstring buffer = L"fish_job_summary";

    // Job id.
    append_format(buffer, L" %d", j->job_id());

    // 1 if foreground, 0 if background.
    append_format(buffer, L" %d", static_cast<int>(j->is_foreground()));

    // Command.
    buffer.push_back(L' ');
    buffer.append(escape_string(j->command(), ESCAPE_ALL));

    if (!p) {
        // No process, we are summarizing the whole job.
        buffer.append(j->is_stopped() ? L" STOPPED" : L" ENDED");
    } else {
        // We are summarizing a process which exited with a signal.
        // Arguments are the signal name and description.
        int sig = p->status.signal_code();
        buffer.push_back(L' ');
        buffer.append(escape_string(sig2wcs(sig), ESCAPE_ALL));

        buffer.push_back(L' ');
        buffer.append(escape_string(signal_get_desc(sig), ESCAPE_ALL));

        // If we have multiple processes, we also append the pid and argv.
        if (j->processes.size() > 1) {
            append_format(buffer, L" %d", p->pid);

            buffer.push_back(L' ');
            buffer.append(escape_string(p->argv0(), ESCAPE_ALL));
        }
    }
    return buffer;
}

// Summarize a list of jobs, by emitting calls to fish_job_summary.
// Note the given list must NOT be the parser's own job list, since the call to fish_job_summary
// could modify it.
static bool summarize_jobs(parser_t &parser, const std::vector<job_ref_t> &jobs) {
    if (jobs.empty()) return false;

    for (const auto &j : jobs) {
        if (j->is_stopped()) {
            call_job_summary(parser, summary_command(j));
        } else {
            // Completed job.
            for (const auto &p : j->processes) {
                if (proc_wants_summary(j, p)) {
                    call_job_summary(parser, summary_command(j, p));
                }
            }

            // Overall status for the job.
            if (job_wants_summary(j)) {
                call_job_summary(parser, summary_command(j));
            }
        }
    }
    return true;
}

/// Remove all disowned jobs whose job chain is fully constructed (that is, do not erase disowned
/// jobs that still have an in-flight parent job). Note we never print statuses for such jobs.
static void remove_disowned_jobs(job_list_t &jobs) {
    auto iter = jobs.begin();
    while (iter != jobs.end()) {
        const auto &j = *iter;
        if (j->flags().disown_requested && j->job_chain_is_fully_constructed()) {
            iter = jobs.erase(iter);
        } else {
            ++iter;
        }
    }
}

/// Given that a job has completed, check if it may be wait'ed on; if so add it to the wait handle
/// store. Then mark all wait handles as complete.
static void save_wait_handle_for_completed_job(const shared_ptr<job_t> &job,
                                               wait_handle_store_t &store) {
    assert(job && job->is_completed() && "Job null or not completed");
    // Are we a background job?
    if (!job->is_foreground()) {
        for (auto &proc : job->processes) {
            store.add(proc->make_wait_handle(job->internal_job_id));
        }
    }

    // Mark all wait handles as complete (but don't create just for this).
    for (auto &proc : job->processes) {
        if (wait_handle_ref_t wh = proc->get_wait_handle()) {
            wh->status = proc->status.status_value();
            wh->completed = true;
        }
    }
}

/// Remove completed jobs from the job list, printing status messages as appropriate.
/// \return whether something was printed.
static bool process_clean_after_marking(parser_t &parser, bool allow_interactive) {
    ASSERT_IS_MAIN_THREAD();

    // This function may fire an event handler, we do not want to call ourselves recursively (to
    // avoid infinite recursion).
    if (parser.libdata().is_cleaning_procs) {
        return false;
    }
    const scoped_push<bool> cleaning(&parser.libdata().is_cleaning_procs, true);

    // This may be invoked in an exit handler, after the TERM has been torn down
    // Don't try to print in that case (#3222)
    const bool interactive = allow_interactive && cur_term != nullptr;

    // Remove all disowned jobs.
    remove_disowned_jobs(parser.jobs());

    // Accumulate exit events into a new list, which we fire after the list manipulation is
    // complete.
    std::vector<event_t> exit_events;

    // Figure out which event types have handlers, so we can avoid the cost of creating events that
    // are unhandled. In principle, an event handler could add a handler for another event type,
    // which won't receive it. We ignore that possibility.
    const enum_set_t<event_type_t> handled_event_types = event_get_handled_types();

    // Defer processing under-construction jobs or jobs that want a message when we are not
    // interactive.
    auto should_process_job = [=](const shared_ptr<job_t> &j) {
        // Do not attempt to process jobs which are not yet constructed.
        // Do not attempt to process jobs that need to print a status message,
        // unless we are interactive, in which case printing is OK.
        return j->is_constructed() && (interactive || !job_or_proc_wants_summary(j));
    };

    // The list of jobs to summarize. Some of these jobs are completed and are removed from the
    // parser's job list, others are stopped and remain in the list.
    std::vector<job_ref_t> jobs_to_summarize;

    // Handle stopped jobs. These stay in our list.
    for (const auto &j : parser.jobs()) {
        if (j->is_stopped() && !j->flags().notified_of_stop && should_process_job(j) &&
            job_wants_summary(j)) {
            j->mut_flags().notified_of_stop = true;
            jobs_to_summarize.push_back(j);
        }
    }

    // Remove completed, processable jobs from our job list.
    for (auto iter = parser.jobs().begin(); iter != parser.jobs().end();) {
        const job_ref_t &j = *iter;
        if (!should_process_job(j) || !j->is_completed()) {
            ++iter;
            continue;
        }
        // We are committed to removing this job.
        // Remember it for summary later, generate exit events, maybe save its wait handle if it
        // finished in the background.
        if (job_or_proc_wants_summary(j)) jobs_to_summarize.push_back(j);
        generate_exit_events(j, handled_event_types, &exit_events);
        save_wait_handle_for_completed_job(j, parser.get_wait_handles());

        // Remove it.
        iter = parser.jobs().erase(iter);
    }

    // Emit calls to fish_job_summary.
    bool printed = summarize_jobs(parser, jobs_to_summarize);

    // Post pending exit events.
    // If we have none, still give any other pending events (e.g. from signals) a chance to run.
    if (exit_events.empty()) {
        event_fire_delayed(parser);
    } else {
        for (const auto &evt : exit_events) {
            event_fire(parser, evt);
        }
    }

    if (printed) {
        fflush(stdout);
    }

    return printed;
}

bool job_reap(parser_t &parser, bool allow_interactive) {
    ASSERT_IS_MAIN_THREAD();
    // Early out for the common case that there are no jobs.
    if (parser.jobs().empty()) {
        return false;
    }

    process_mark_finished_children(parser, false /* not block_ok */);
    return process_clean_after_marking(parser, allow_interactive);
}

/// Get the CPU time for the specified process.
unsigned long proc_get_jiffies(pid_t inpid) {
    if (inpid <= 0 || !have_proc_stat()) return 0;

    char state;
    int pid, ppid, pgrp, session, tty_nr, tpgid, exit_signal, processor;
    long int cutime, cstime, priority, nice, placeholder, itrealvalue, rss;
    unsigned long int flags, minflt, cminflt, majflt, cmajflt, utime, stime, starttime, vsize, rlim,
        startcode, endcode, startstack, kstkesp, kstkeip, signal, blocked, sigignore, sigcatch,
        wchan, nswap, cnswap;
    char comm[1024];

    /// Maximum length of a /proc/[PID]/stat filename.
    constexpr size_t FN_SIZE = 256;
    char fn[FN_SIZE];
    std::snprintf(fn, FN_SIZE, "/proc/%d/stat", inpid);
    // Don't use autoclose_fd here, we will fdopen() and then fclose() instead.
    int fd = open_cloexec(fn, O_RDONLY);
    if (fd < 0) return 0;

    // TODO: replace the use of fscanf() as it is brittle and should never be used.
    FILE *f = fdopen(fd, "r");
    int count = fscanf(f,
                       "%9d %1023s %c %9d %9d %9d %9d %9d %9lu "
                       "%9lu %9lu %9lu %9lu %9lu %9lu %9ld %9ld %9ld "
                       "%9ld %9ld %9ld %9lu %9lu %9ld %9lu %9lu %9lu "
                       "%9lu %9lu %9lu %9lu %9lu %9lu %9lu %9lu %9lu "
                       "%9lu %9d %9d ",
                       &pid, comm, &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags, &minflt,
                       &cminflt, &majflt, &cmajflt, &utime, &stime, &cutime, &cstime, &priority,
                       &nice, &placeholder, &itrealvalue, &starttime, &vsize, &rss, &rlim,
                       &startcode, &endcode, &startstack, &kstkesp, &kstkeip, &signal, &blocked,
                       &sigignore, &sigcatch, &wchan, &nswap, &cnswap, &exit_signal, &processor);
    fclose(f);
    if (count < 17) return 0;
    return utime + stime + cutime + cstime;
}

/// Update the CPU time for all jobs.
void proc_update_jiffies(parser_t &parser) {
    for (const auto &job : parser.jobs()) {
        for (process_ptr_t &p : job->processes) {
            p->last_time = timef();
            p->last_jiffies = proc_get_jiffies(p->pid);
        }
    }
}

// Return control of the terminal to a job's process group. restore_attrs is true if we are
// restoring a previously-stopped job, in which case we need to restore terminal attributes.
int terminal_maybe_give_to_job_group(const job_group_t *jg, bool continuing_from_stopped) {
    enum { notneeded = 0, success = 1, error = -1 };

    if (!jg->should_claim_terminal()) {
        // The job doesn't want the terminal.
        return notneeded;
    }

    // Get the pgid; we may not have one.
    pid_t pgid{};
    if (auto mpgid = jg->get_pgid()) {
        pgid = *mpgid;
    } else {
        FLOG(proc_termowner, L"terminal_give_to_job() returning early due to no process group");
        return notneeded;
    }

    // If we are continuing, ensure that stdin is marked as blocking first (issue #176).
    // Also restore tty modes.
    if (continuing_from_stopped) {
        make_fd_blocking(STDIN_FILENO);
        if (jg->tmodes.has_value()) {
            int res = tcsetattr(STDIN_FILENO, TCSADRAIN, &jg->tmodes.value());
            if (res < 0) wperror(L"tcsetattr");
        }
    }

    // Ok, we want to transfer to the child.
    // Note it is important to be very careful about calling tcsetpgrp()!
    // fish ignores SIGTTOU which means that it has the power to reassign the tty even if it doesn't
    // own it. This means that other processes may get SIGTTOU and become zombies.
    // Check who own the tty now. Thre's five cases of interest:
    //   1. The process's pgrp is the same as fish. In that case there is nothing to do.
    //   2. There is no tty at all (tcgetpgrp() returns -1). For example running from a pure script.
    //      Of course do not transfer it in that case.
    //   3. The tty is owned by the process. This comes about often, as the process will call
    //      tcsetpgrp() on itself between fork ane exec. This is the essential race inherent in
    //      tcsetpgrp(). In this case we want to reclaim the tty, but do not need to transfer it
    //      ourselves since the child won the race.
    //   4. The tty is owned by a different process. This may come about if fish is running in the
    //      background with job control enabled. Do not transfer it.
    //   5. The tty is owned by fish. In that case we want to transfer the pgid.
    pid_t fish_pgrp = getpgrp();
    if (fish_pgrp == pgid) {
        // Case 1.
        return notneeded;
    }
    pid_t current_owner = tcgetpgrp(STDIN_FILENO);
    if (current_owner < 0) {
        // Case 2.
        return notneeded;
    } else if (current_owner == pgid) {
        // Case 3.
        return success;
    } else if (current_owner != pgid && current_owner != fish_pgrp) {
        // Case 4.
        return notneeded;
    }
    // Case 5 - we do want to transfer it.

    // The tcsetpgrp(2) man page says that EPERM is thrown if "pgrp has a supported value, but
    // is not the process group ID of a process in the same session as the calling process."
    // Since we _guarantee_ that this isn't the case (the child calls setpgid before it calls
    // SIGSTOP, and the child was created in the same session as us), it seems that EPERM is
    // being thrown because of an caching issue - the call to tcsetpgrp isn't seeing the
    // newly-created process group just yet. On this developer's test machine (WSL running Linux
    // 4.4.0), EPERM does indeed disappear on retry. The important thing is that we can
    // guarantee the process isn't going to exit while we wait (which would cause us to possibly
    // block indefinitely).
    while (tcsetpgrp(STDIN_FILENO, pgid) != 0) {
        FLOGF(proc_termowner, L"tcsetpgrp failed: %d", errno);

        // Before anything else, make sure that it's even necessary to call tcsetpgrp.
        // Since it usually _is_ necessary, we only check in case it fails so as to avoid the
        // unnecessary syscall and associated context switch, which profiling has shown to have
        // a significant cost when running process groups in quick succession.
        int getpgrp_res = tcgetpgrp(STDIN_FILENO);
        if (getpgrp_res < 0) {
            switch (errno) {
                case ENOTTY:
                    // stdin is not a tty. This may come about if job control is enabled but we are
                    // not a tty - see #6573.
                    return notneeded;
                case EBADF:
                    // stdin has been closed. Workaround a glibc bug - see #3644.
                    redirect_tty_output();
                    return notneeded;
                default:
                    wperror(L"tcgetpgrp");
                    return error;
            }
        }
        if (getpgrp_res == pgid) {
            FLOGF(proc_termowner, L"Process group %d already has control of terminal", pgid);
            return notneeded;
        }

        bool pgroup_terminated = false;
        if (errno == EINVAL) {
            // OS X returns EINVAL if the process group no longer lives. Probably other OSes,
            // too. Unlike EPERM below, EINVAL can only happen if the process group has
            // terminated.
            pgroup_terminated = true;
        } else if (errno == EPERM) {
            // Retry so long as this isn't because the process group is dead.
            int wait_result = waitpid(-1 * pgid, &wait_result, WNOHANG);
            if (wait_result == -1) {
                // Note that -1 is technically an "error" for waitpid in the sense that an
                // invalid argument was specified because no such process group exists any
                // longer. This is the observed behavior on Linux 4.4.0. a "success" result
                // would mean processes from the group still exist but is still running in some
                // state or the other.
                pgroup_terminated = true;
            } else {
                // Debug the original tcsetpgrp error (not the waitpid errno) to the log, and
                // then retry until not EPERM or the process group has exited.
                FLOGF(proc_termowner, L"terminal_give_to_job(): EPERM.\n", pgid);
                continue;
            }
        } else if (errno == ENOTTY) {
            // stdin is not a TTY. In general we expect this to be caught via the tcgetpgrp
            // call's EBADF handler above.
            return notneeded;
        } else {
            FLOGF(warning, _(L"Could not send job %d ('%ls') with pgid %d to foreground"),
                  jg->get_id(), jg->get_command().c_str(), pgid);
            wperror(L"tcsetpgrp");
            return error;
        }

        if (pgroup_terminated) {
            // All processes in the process group has exited.
            // Since we delay reaping any processes in a process group until all members of that
            // job/group have been started, the only way this can happen is if the very last
            // process in the group terminated and didn't need to access the terminal, otherwise
            // it would have hung waiting for terminal IO (SIGTTIN). We can safely ignore this.
            FLOGF(proc_termowner, L"tcsetpgrp called but process group %d has terminated.\n", pgid);
            return notneeded;
        }

        break;
    }

    return success;
}

/// Returns control of the terminal to the shell, and saves the terminal attribute state to the job
/// group, so that we can restore the terminal ownership to the job at a later time.
static bool terminal_return_from_job_group(job_group_t *jg) {
    errno = 0;
    auto pgid = jg->get_pgid();
    if (!pgid.has_value()) {
        FLOG(proc_pgroup, "terminal_return_from_job() returning early due to no process group");
        return true;
    }

    FLOG(proc_pgroup, "fish reclaiming terminal after job pgid", *pgid);
    if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1) {
        if (errno == ENOTTY) redirect_tty_output();
        FLOGF(warning, _(L"Could not return shell to foreground"));
        wperror(L"tcsetpgrp");
        return false;
    }

    // Save jobs terminal modes.
    struct termios tmodes {};
    if (tcgetattr(STDIN_FILENO, &tmodes)) {
        // If it's not a tty, it's not a tty, and there are no attributes to save (or restore)
        if (errno == ENOTTY) return false;
        FLOGF(warning, _(L"Could not return shell to foreground"));
        wperror(L"tcgetattr");
        return false;
    }
    jg->tmodes = tmodes;
    return true;
}

bool job_t::is_foreground() const { return group->is_foreground(); }

maybe_t<pid_t> job_t::get_pgid() const { return group->get_pgid(); }

maybe_t<pid_t> job_t::get_last_pid() const {
    for (auto iter = processes.rbegin(); iter != processes.rend(); ++iter) {
        const process_t *proc = iter->get();
        if (proc->pid > 0) return proc->pid;
    }
    return none();
}

job_id_t job_t::job_id() const { return group->get_id(); }

void job_t::continue_job(parser_t &parser, bool in_foreground) {
    // Put job first in the job list.
    parser.job_promote(this);
    mut_flags().notified_of_stop = false;

    int pgid = -2;
    if (auto tmp = get_pgid()) pgid = *tmp;

    // We must send_sigcont if the job is stopped.
    bool send_sigcont = this->is_stopped();

    FLOGF(proc_job_run, L"%ls job %d, gid %d (%ls), %ls, %ls",
          send_sigcont ? L"Continue" : L"Start", job_id(), pgid, command_wcstr(),
          is_completed() ? L"COMPLETED" : L"UNCOMPLETED",
          parser.libdata().is_interactive ? L"INTERACTIVE" : L"NON-INTERACTIVE");

    // Make sure we retake control of the terminal before leaving this function.
    bool term_transferred = false;
    cleanup_t take_term_back([&] {
        if (term_transferred) {
            // Issues of interest include #121 and #2114.
            terminal_return_from_job_group(this->group.get());
        }
    });

    if (!is_completed()) {
        int transfer = terminal_maybe_give_to_job_group(this->group.get(), send_sigcont);
        if (transfer < 0) {
            // terminal_maybe_give_to_job prints an error.
            return;
        }
        term_transferred = (transfer > 0);

        // If both requested and necessary, send the job a continue signal.
        if (send_sigcont) {
            // This code used to check for JOB_CONTROL to decide between using killpg to signal all
            // processes in the group or iterating over each process in the group and sending the
            // signal individually. job_t::signal() does the same, but uses the shell's own pgroup
            // to make that distinction.
            if (!signal(SIGCONT)) {
                FLOGF(proc_pgroup, "Failed to send SIGCONT to any processes in pgroup %d!", pgid);
                // This returns without bubbling up the error. Presumably that is OK.
                return;
            }

            // reset the status of each process instance
            for (auto &p : processes) {
                p->stopped = false;
            }
        }

        if (in_foreground) {
            // Wait for the status of our own job to change.
            while (!check_cancel_from_fish_signal() && !is_stopped() && !is_completed()) {
                process_mark_finished_children(parser, true);
            }
        }
    }

    if (in_foreground && is_completed()) {
        // Set $status only if we are in the foreground and the last process in the job has
        // finished.
        const auto &p = processes.back();
        if (p->status.normal_exited() || p->status.signal_exited()) {
            auto statuses = get_statuses();
            if (statuses) {
                parser.set_last_statuses(statuses.value());
                parser.libdata().status_count++;
            }
        }
    }
}

void proc_wait_any(parser_t &parser) {
    process_mark_finished_children(parser, true /* block_ok */);
    process_clean_after_marking(parser, parser.libdata().is_interactive);
}

void hup_jobs(const job_list_t &jobs) {
    pid_t fish_pgrp = getpgrp();
    for (const auto &j : jobs) {
        auto pgid = j->get_pgid();
        if (pgid && *pgid != fish_pgrp && !j->is_completed()) {
            if (j->is_stopped()) {
                j->signal(SIGCONT);
            }
            j->signal(SIGHUP);
        }
    }
}

static std::atomic<bool> s_is_within_fish_initialization{false};

void set_is_within_fish_initialization(bool flag) { s_is_within_fish_initialization.store(flag); }

bool is_within_fish_initialization() { return s_is_within_fish_initialization.load(); }
