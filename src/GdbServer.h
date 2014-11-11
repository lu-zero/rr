/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_GDB_SERVER_H_
#define RR_GDB_SERVER_H_

#include <map>
#include <memory>
#include <string>

#include "DiversionSession.h"
#include "GdbConnection.h"
#include "ReplaySession.h"
#include "ScopedFd.h"
#include "TraceFrame.h"

class GdbServer {
public:
  struct Target {
    Target() : pid(0), require_exec(true), event(0) {}
    // Target process to debug, or 0 to just debug the first process
    pid_t pid;
    // If true, wait for the target process to exec() before attaching debugger
    bool require_exec;
    // Wait until at least 'event' has elapsed before attaching
    TraceFrame::Time event;
  };

  /**
   * Create a gdbserver serving the replay of 'trace_dir'.
   * If debugger_params_write_pipe is non-null, write gdb launch parameters
   * to the pipe; another process should call launch_gdb with the other end
   * of the pipe to exec() gdb.
   */
  static void serve(const std::string& trace_dir, const Target& target,
                    ScopedFd* debugger_params_write_pipe = nullptr) {
    GdbServer(target).serve_replay(trace_dir, debugger_params_write_pipe);
  }

  /**
   * exec()'s gdb using parameters read from params_pipe_fd (and sent through
   * the pipe passed to serve_replay_with_debugger).
   */
  static void launch_gdb(ScopedFd& params_pipe_fd);

  /**
   * Start a debugging connection for |t| and return when there are no
   * more requests to process (usually because the debugger detaches).
   *
   * This helper doesn't attempt to determine whether blocking rr on a
   * debugger connection might be a bad idea.  It will always open the debug
   * socket and block awaiting a connection.
   */
  static void emergency_debug(Task* t);

private:
  GdbServer(const Target& target)
      : target(target), debugger_active(false), diversion_refcount(0) {}
  GdbServer(std::unique_ptr<GdbConnection>& dbg)
      : dbg(std::move(dbg)), debugger_active(true), diversion_refcount(0) {}

  void maybe_singlestep_for_event(Task* t, GdbRequest* req);
  /**
   * If |req| is a magic-write command, interpret it and return true.
   * Otherwise, do nothing and return false.
   */
  bool maybe_process_magic_command(Task* t, const GdbRequest& req);
  /**
   * Process the single debugger request |req|, made by |dbg| targeting
   * |t|, inside the session |session|.
   *
   * Callers should implement any special semantics they want for
   * particular debugger requests before calling this helper, to do
   * generic processing.
   */
  void dispatch_debugger_request(Session& session, Task* t,
                                 const GdbRequest& req);
  /**
   * If the trace has reached the event at which the user wanted a debugger
   * started, then create one and store it in `dbg` if we don't already
   * have one there, and return true. Otherwise return false.
   *
   * This must be called before scheduling the task for the next event
   * (and thereby mutating the TraceIfstream for that event).
   */
  bool maybe_connect_debugger(ScopedFd* debugger_params_write_pipe);
  void maybe_restart_session(const GdbRequest& req);
  GdbRequest process_debugger_requests(Task* t);
  GdbRequest replay_one_step();
  void serve_replay(const std::string& trace_dir,
                    ScopedFd* debugger_params_write_pipe);

  /**
   * Process debugger requests made through |dbg| in
   * |diversion_session| until action needs to
   * be taken by the caller (a resume-execution request is received).
   * The returned Task* is the target of the resume-execution request.
   *
   * The received request is returned through |req|.
   */
  Task* diverter_process_debugger_requests(Task* t, GdbRequest* req);
  /**
   * Create a new diversion session using |replay| session as the
   * template.  The |replay| session isn't mutated.
   *
   * Execution begins in the new diversion session under the control of
   * |dbg| starting with initial thread target |task|.  The diversion
   * session ends at the request of |dbg|, and |divert| returns the first
   * request made that wasn't handled by the diversion session.  That
   * is, the first request that should be handled by |replay| upon
   * resuming execution in that session.
   */
  GdbRequest divert(ReplaySession& replay, pid_t task);

  /**
   * Return the checkpoint stored as |checkpoint_id| or nullptr if there
   * isn't one.
   */
  ReplaySession::shr_ptr get_checkpoint(int checkpoint_id);
  /**
   * Delete the checkpoint stored as |checkpoint_id| if it exists, or do
   * nothing if it doesn't exist.
   */
  void delete_checkpoint(int checkpoint_id);

  Target target;
  std::unique_ptr<GdbConnection> dbg;
  // False while we're waiting for the session to reach some requested state
  // before talking to gdb.
  bool debugger_active;

  // |session| is used to drive replay.
  ReplaySession::shr_ptr session;
  // If we're being controlled by a debugger, then |last_debugger_start| is
  // the saved session we forked 'session' from.
  ReplaySession::shr_ptr debugger_restart_checkpoint;
  // Checkpoints, indexed by checkpoint ID
  std::map<int, ReplaySession::shr_ptr> checkpoints;

  // The diversion session.
  DiversionSession::shr_ptr diversion_session;
  // Number of client references to diversion_session.
  // When there are 0 refs it is considered to be dying.
  int diversion_refcount;
};

#endif /* RR_GDB_SERVER_H_ */
