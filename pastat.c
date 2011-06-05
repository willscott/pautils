//
// Watch the amplitude of audio flowing to your sinks
//
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/// Function Declarations
void pa_source_cb(pa_context *c, const pa_source_info *i, int last,
    void *userdata);
void pa_read_cb(pa_stream *s, size_t len, void *userdata);
void pa_state_cb(pa_context *c, void *userdata);
void pretty_print(short amp);
int pa_stat(int samples, char *sink, short quiet);

struct monitored_stream {
  pa_stream *stream;
  short amp;
  char *source_name;
  int source_index;
  struct monitored_stream *next;
};

struct stream_stats {
  struct monitored_stream *head;
  int count;
  pa_context *ctx;
  pa_sample_spec *spec;
  char *filter;
};

/// Function Implementations
void pretty_print(short s) {
  float val = SHRT_MAX;
  if (s < 0) {
    fprintf(stdout,"-\t");
  } else {
    val = s * 100.0 / val;
    fprintf(stdout,"%2.2f%%\t",val);
  }
}

void pa_state_cb(pa_context *c, void *userdata) {
  int *pa_ready = userdata;

  switch (pa_context_get_state(c)) {
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      *pa_ready = 2;
      break;
    case PA_CONTEXT_READY:
      *pa_ready = 1;
      break;
    default:
      break;
  }
}

// Listen for sources, create streams for monitoring.
void pa_source_cb(pa_context *c, const pa_source_info *i, int last,
    void *userdata) {
  struct stream_stats *stats = userdata;

  if (last) {
    return;
  }

  if (i->monitor_of_sink_name && (stats->filter == NULL ||
      strstr(i->description, stats->filter))) {
    // New stream allocation.
    struct monitored_stream* watch =
        (struct monitored_stream*)malloc(sizeof(struct monitored_stream));
    watch->stream = pa_stream_new(stats->ctx, "pastat", stats->spec, NULL);
    pa_stream_connect_record(watch->stream, i->name, NULL, 0);
    pa_stream_set_read_callback(watch->stream, pa_read_cb, watch);
    watch->source_name = pa_xstrdup(i->description);
    watch->source_index = i->index;
    watch->amp = -1;
    watch->next = stats->head;
    stats->head = watch;
    stats->count += 1;
  }
}

// Listens for available stream data
void pa_read_cb(pa_stream *s, size_t len, void *userdata) {
  struct monitored_stream* watch = userdata;
  const void *data;
  size_t slen;
  size_t pos;
  short max = watch->amp;
  short sample = 0;
  if (pa_stream_peek(s, &data, &len) < 0) {
    fprintf(stderr, "Reading stream failed.\n");
    return;
  }

  slen = len / sizeof(short);
  for (pos = 0; pos < slen; pos+=1) {
    sample = ((short*)data)[pos];
    if (sample > max) {
      max = sample;
    } else if (sample < -max) {
      max = -sample;
    }
  }

  watch->amp = max;
  pa_stream_drop(s);
}

int pa_stat(int samples, char *sink, short quiet) {
  // Connection variables
  pa_mainloop *pa_ml;
  pa_mainloop_api *pa_mlapi;
  pa_operation *pa_op;
  pa_context *pa_ctx;
  static pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2
  };

  // Request State Variables
  int state = 0;
  int pa_ready = 0;
  struct stream_stats streams;
  streams.filter = sink;
  streams.count = 0;
  streams.head = NULL;
  streams.spec = &ss;
  struct monitored_stream* a_stream = streams.head;
  time_t now = time(NULL);

  // Initiate Connection
  pa_ml = pa_mainloop_new();
  pa_mlapi = pa_mainloop_get_api(pa_ml);
  pa_ctx = pa_context_new(pa_mlapi, "pastat");
  streams.ctx = pa_ctx;
  pa_context_connect(pa_ctx, NULL, 0, NULL);

  // Request server state.
  pa_context_set_state_callback(pa_ctx, pa_state_cb, &pa_ready);

  // Mainloop
  while (1) {
    if (pa_ready == 0) {
      pa_mainloop_iterate(pa_ml, 1, NULL);
      continue;
    } else if (pa_ready == 2) {
      pa_context_disconnect(pa_ctx);
      pa_context_unref(pa_ctx);
      pa_mainloop_free(pa_ml);
      fprintf(stderr, "Failed to connect to pulse audio\n");
      return 1;
    } else if (pa_ready == 3) {
      // Data structure cleanup.
      if (streams.head != NULL) {
        a_stream = streams.head;
        while(a_stream->next != NULL) {
          pa_xfree(a_stream->source_name);
          pa_stream_disconnect(a_stream->stream);
          pa_stream_unref(a_stream->stream);
          struct monitored_stream* next = a_stream->next;
          free(a_stream);
          a_stream = next;
        }
        pa_stream_disconnect(a_stream->stream);
        pa_stream_unref(a_stream->stream);
        pa_xfree(a_stream->source_name);
        free(a_stream);
      }
      pa_context_disconnect(pa_ctx);
      pa_context_unref(pa_ctx);
      pa_mainloop_free(pa_ml);
      return 0;
    }

    switch (state) {
      // State 0: Just Connected to the server.
      case 0:
	pa_op = pa_context_get_source_info_list(pa_ctx, pa_source_cb, &streams);
	state++;
        break;
      // State 1: Sources requested from server.
      case 1:
        if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
          pa_operation_unref(pa_op);
          state++;
        }
        break;
      // State 2: Streams listening for data, print headers.
      case 2:
        if (quiet == 0) {
          unsigned int idx = 0;
          a_stream = streams.head;
          while(a_stream != NULL) {
            fprintf(stdout,"[%u] %s\n", idx++, a_stream->source_name);
            a_stream = a_stream->next;
          }
          fprintf(stdout, "\n");
          idx = 0;
          a_stream = streams.head;
          while(a_stream != NULL) {
            fprintf(stdout,"[%u]\t", idx++);
            a_stream = a_stream->next;
          }
          fprintf(stdout, "\n");
        }
        state++;
        break;
      // Print out samples.
      case 3:
        if (now < time(NULL)) {
          now = time(NULL);
          samples--;
          a_stream = streams.head;
          while(a_stream != NULL) {
            pretty_print(a_stream->amp);
            a_stream->amp = -1;
            a_stream = a_stream->next;
          }
          fprintf(stdout, "\n");
        }
        if (samples == 0) {
          pa_ready = 3;
          continue;
        }
        break;
      default:
        pa_ready = 2;
        continue;
    }
    pa_mainloop_iterate(pa_ml, 1, NULL);
  }
  return 0;
}

int main(int argc, char **argv) {
  int n = -1;
  char *sink = NULL;
  int c;
  short q = 0;
  while ((c = getopt (argc, argv, "n:s:q")) != -1) {
    switch (c) {
      case 'n':
        n = atoi(optarg);
        break;
      case 's':
        sink = optarg;
        break;
      case 'q':
        q = 1;
        break;
      case '?':
      default:
        fprintf (stderr, "Usage: %s [-q] [-n <Number of samples>] [-s <Sink>].\n",
            argv[0]);
        return 1;
    }
  }
  n = pa_stat(n, sink, q);
  return n;
}
