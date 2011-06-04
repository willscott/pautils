//
// Given a process, toggle the sink device for all audio streams associated with
// that process.
//
#include <stdio.h>
#include <string.h>
#include <pulse/pulseaudio.h>

void pa_state_cb(pa_context *c, void *userdata);
void pa_sinklist_cb(pa_context *c, const pa_sink_info *l, int eol, void *userdata);
void pa_clientlist_cb(pa_context *c, const pa_client_info *l, int eol, void *userdata);
void pa_streamlist_cb(pa_context *c, const pa_sink_input_info *l, int eol, void *userdata);
void pa_move_cb(pa_context *c, int success, void *userdata);

int pa_toggle_pid(int pid);

struct pa_target {
  int pid;
  int num_sinks;
  int client_index;
  short stream_found;
  int stream_index;
  int stream_sink;
  int status;
};

// This callback gets called when our context changes state.  We really only
// care about when it's ready or if it has failed
void pa_state_cb(pa_context *c, void *userdata) {
	pa_context_state_t state;
	int *pa_ready = userdata;

	state = pa_context_get_state(c);
	switch  (state) {
		default:
			break;
		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			*pa_ready = 2;
			break;
		case PA_CONTEXT_READY:
			*pa_ready = 1;
			break;
	}
}

// Record sinks the server tells us about
void pa_sinklist_cb(pa_context *c, const pa_sink_info *l, int eol, void *userdata) {
  // ignore the final entry.
  if (eol > 0) {
    return;
  }

  struct pa_target *target = userdata;
  target->num_sinks++;
}

// Handle clients the server tells us about.
void pa_clientlist_cb(pa_context *c, const pa_client_info *l, int eol, void *userdata) {
  struct pa_target *target = userdata;
  if (eol > 0) {
    return;
  }

  char* pidkey = "application.process.id";
  if (pa_proplist_contains(l->proplist, pidkey)) {
    int cpid = atoi(pa_proplist_gets(l->proplist, pidkey));
    if (target->pid == cpid) {
      target->client_index = l->index;
    }
  }
}

// Handle streams the server tells us about.
void pa_streamlist_cb(pa_context *c, const pa_sink_input_info *l, int eol, void *userdata) {
  struct pa_target *target = userdata;
  if (eol > 0) {
    return;
  }

  if (l->client == target->client_index) {
    target->stream_found = 1;
    target->stream_index = l->index;
    target->stream_sink = l->sink;
  }
}

// Handle result of the move.
void pa_move_cb(pa_context *c, int success, void *userdata) {
  struct pa_target *target = userdata;
  target->status = success;  
}

int pa_toggle_pid(int pid) {
    // Define our pulse audio loop and connection variables
    pa_mainloop *pa_ml;
    pa_mainloop_api *pa_mlapi;
    pa_operation *pa_op;
    pa_context *pa_ctx;

    // We'll need these state variables to keep track of our requests
    int state = 0;
    int pa_ready = 0;
    struct pa_target target;
    target.num_sinks = 0;
    target.pid = pid;
    target.stream_found = 0;

    // Create a mainloop API and connection to the default server
    pa_ml = pa_mainloop_new();
    pa_mlapi = pa_mainloop_get_api(pa_ml);
    pa_ctx = pa_context_new(pa_mlapi, "streamtoggler");
    pa_context_connect(pa_ctx, NULL, 0, NULL);

    // Listen to server state.
    pa_context_set_state_callback(pa_ctx, pa_state_cb, &pa_ready);

    // Mainloop
    for (;;) {
	if (pa_ready == 0) {
	    pa_mainloop_iterate(pa_ml, 1, NULL);
	    continue;
	}
	// We couldn't get a connection to the server, so exit out
	if (pa_ready == 2) {
	    pa_context_disconnect(pa_ctx);
	    pa_context_unref(pa_ctx);
	    pa_mainloop_free(pa_ml);
	    fprintf(stderr, "failed to connect to pulse audio\n");
	    return -1;
	}

	switch (state) {
	    // State 0: we haven't done anything yet. Request sinks.
	    case 0:
		pa_op = pa_context_get_sink_info_list(pa_ctx,
			pa_sinklist_cb,
			&target);
		state++;
		break;
	    case 1:
		// State 1: wait for sinks to finish.  Request clients.
		if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
		    pa_operation_unref(pa_op);

		    pa_op = pa_context_get_client_info_list(pa_ctx,
			    pa_clientlist_cb,
			    &target);
		    state++;
		}
		break;
	    case 2:
		// State 2: wait for clients to finish. Request streams.
		if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
		    pa_operation_unref(pa_op);

		    pa_op = pa_context_get_sink_input_info_list(pa_ctx,
			    pa_streamlist_cb,
			    &target);
		    state++;
		}
		break;
	    case 3:
		// State 3: wait for streams to finish. Toggle.
		if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
		    if (target.stream_found) {
		      target.stream_sink++;
		      if (target.stream_sink >= target.num_sinks) {
		        target.stream_sink = 0;
		      }
		      pa_operation_unref(pa_op);
		      pa_op = pa_context_move_sink_input_by_index(pa_ctx,
		        target.stream_index,
		        target.stream_sink,
		        pa_move_cb,
		        &target);
		    } else {
			fprintf(stderr, "No Stream found for given pid\n");
		    }
		    state++;
		}
		break;
	    case 4:
	    	// State 4: Wait for the move to finish.  Clean up.
		if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
		    pa_operation_unref(pa_op);
		    pa_context_disconnect(pa_ctx);
		    pa_context_unref(pa_ctx);
		    pa_mainloop_free(pa_ml);
		    return 0;
		}
		break;
	    default:
		// We should never see this state
		fprintf(stderr, "in state %d\n", state);
		return -1;
	}
	// Iterate the main loop and go again.  The second argument is whether
	// or not the iteration should block until something is ready to be
	// done.  Set it to zero for non-blocking.
	pa_mainloop_iterate(pa_ml, 1, NULL);
    }
}

// This program takes a pid,
// finds a stream whose client is associated with that pid, and moves
// it to the next sink index.
int main(int argc, char *argv[]) {
    if (argc < 2) {
      fprintf(stderr, "Usage: %s <Pid>\n", argv[0]);
      return 0;
    }
    int pid = atoi(argv[1]);

    if (pa_toggle_pid(pid) < 0) {
	return 1;
    }
    return 0;
}
