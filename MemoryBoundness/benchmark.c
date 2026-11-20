/*
 * benchmark.c
 *
 * Runs a target program N times under `perf stat`, collecting values for
 * store-buffer-related hardware counters, then reports mean + 95% confidence
 * interval for each event.
 *
 * Build:
 *     gcc -O3 -o benchmark benchmark.c -lm
 *
 * Usage:
 *     ./benchmark --cmd "./your_program arg1 arg2" --runs 1000 \
 *         [--events
 * resource_stalls.sb:u,ld_blocks.store_forward:u,ld_blocks_partial.address_alias:u,ld_blocks.no_sr:u]
 *
 * Notes:
 *   - Requires `perf` installed and working (event names must already be
 *     confirmed valid on your CPU, e.g. via `perf list`).
 *   - Parses perf's CSV output (`perf stat -x ,`), which is far more robust
 *     to parse than the default padded columnar format.
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_EVENTS 16
#define MAX_EVENT_LEN 128
#define MAX_LINE_LEN 1024
#define MAX_CMD_LEN 2048

/* z-score for a 95% two-sided confidence interval (normal approximation,
 * appropriate once n is at least a few dozen -- we default to n=1000) */
#define T_95 1.962

typedef struct {
    char name[MAX_EVENT_LEN];
    double* values;
    int count;
    int capacity;
} EventSamples;

static void die(const char* msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(EXIT_FAILURE);
}

static void event_samples_init(EventSamples* es, const char* name,
                               int capacity) {
    strncpy(es->name, name, MAX_EVENT_LEN - 1);
    es->name[MAX_EVENT_LEN - 1] = '\0';
    es->values = malloc(sizeof(double) * capacity);
    if (!es->values) {
        die("out of memory allocating sample array");
    }
    es->count = 0;
    es->capacity = capacity;
}

static void event_samples_push(EventSamples* es, double v) {
    if (es->count >= es->capacity) {
        es->capacity *= 2;
        es->values = realloc(es->values, sizeof(double) * es->capacity);
        if (!es->values) {
            die("out of memory growing sample array");
        }
    }
    es->values[es->count++] = v;
}

static int find_event_index(EventSamples* events, int n_events,
                            const char* name) {
    for (int i = 0; i < n_events; i++) {
        if (strcmp(events[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Runs `perf stat -x, -e <events> -- <cmd>` once, writes perf's stderr
 * output (the CSV data) to a temp file, parses it, and pushes one value
 * per event into `events`. Returns 0 on success, -1 if any event is
 * missing / <not counted> / <not supported>.
 */
static int run_once(const char* cmd, const char* events_csv,
                    EventSamples* events, int n_events) {
    char tmpfile[] = "/tmp/perf_out_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd == -1) {
        die("could not create temp file");
    }
    close(fd);

    char full_cmd[MAX_CMD_LEN];
    snprintf(full_cmd, sizeof(full_cmd), "perf stat -x, -e %s -- %s 2> %s",
             events_csv, cmd, tmpfile);

    int rc = system(full_cmd);
    if (rc != 0) {
        fprintf(stderr, "WARNING: command exited with status %d\n", rc);
    }

    FILE* f = fopen(tmpfile, "r");
    if (!f) {
        die("could not open perf temp output");
    }

    /* Parse into a local per-run buffer first. We only commit (push) into
     * the EventSamples arrays once we know the ENTIRE run succeeded --
     * otherwise a run that fails partway through would push values for
     * the events parsed before the failure but not the ones after it,
     * silently misaligning every EventSamples array against each other. */
    double run_values[MAX_EVENTS];
    int found[MAX_EVENTS] = {0};

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        /* CSV format (with -x,): value,unit,event,run_time,pct,...
         * NOTE: we deliberately do NOT use strtok() here -- strtok treats
         * consecutive delimiters as one and silently skips empty fields
         * (e.g. the empty "unit" field in "1265,,resource_stalls.sb,...."),
         * which would shift every subsequent field index left. We need a
         * split that preserves empty fields, so we walk the string by hand. */
        char linecopy[MAX_LINE_LEN];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char* fields[8] = {0};
        int nfields = 0;
        char* p = linecopy;
        fields[nfields++] = p;
        while (*p && nfields < 8) {
            if (*p == ',') {
                *p = '\0';
                fields[nfields++] = p + 1;
            }
            p++;
        }
        if (nfields < 3) {
            continue;
        }

        char* raw_value = fields[0];
        char* event_name = fields[2];

        /* trim whitespace/newlines */
        char* end;
        for (end = event_name + strlen(event_name) - 1;
             end >= event_name && (*end == '\n' || *end == '\r' || *end == ' ');
             end--) {
            *end = '\0';
        }

        int idx = find_event_index(events, n_events, event_name);
        if (idx == -1) {
            continue;
        }

        if (strstr(raw_value, "<not counted>") ||
            strstr(raw_value, "<not supported>")) {
            fclose(f);
            remove(tmpfile);
            fprintf(stderr, "WARNING: event '%s' returned '%s'\n", event_name,
                    raw_value);
            return -1;
        }

        double val = atof(raw_value);
        run_values[idx] = val;
        found[idx] = 1;
    }
    fclose(f);
    remove(tmpfile);

    for (int i = 0; i < n_events; i++) {
        if (!found[i]) {
            fprintf(stderr, "WARNING: missing event '%s' in perf output\n",
                    events[i].name);
            return -1;
        }
    }

    /* Whole run succeeded -- now it's safe to commit all values, keeping
     * every EventSamples array the same length after this call returns. */
    for (int i = 0; i < n_events; i++) {
        event_samples_push(&events[i], run_values[i]);
    }
    return 0;
}

static void confidence_interval_95(double* vals, int n, double* mean_out,
                                   double* half_width_out) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += vals[i];
    }
    double mean = sum / n;

    if (n < 2) {
        *mean_out = mean;
        *half_width_out = NAN;
        return;
    }

    double sq_sum = 0.0;
    for (int i = 0; i < n; i++) {
        double d = vals[i] - mean;
        sq_sum += d * d;
    }
    double stdev = sqrt(sq_sum / (n - 1)); /* sample stdev, n-1 denominator */
    double sem = stdev / sqrt((double)n);

    *mean_out = mean;
    *half_width_out = T_95 * sem;
}

static double stdev_of(double* vals, int n, double mean) {
    if (n < 2) {
        return NAN;
    }
    double sq_sum = 0.0;
    for (int i = 0; i < n; i++) {
        double d = vals[i] - mean;
        sq_sum += d * d;
    }
    return sqrt(sq_sum / (n - 1));
}

int main(int argc, char** argv) {
    const char* cmd = NULL;
    int runs = 1000;
    const char* events_csv =
        //"resource_stalls.sb:u,
        "ld_blocks.store_forward:u,ld_blocks_partial."
        "address_alias:u,ld_blocks.no_sr:u";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cmd") == 0 && i + 1 < argc) {
            cmd = argv[++i];
        } else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            runs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
            events_csv = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            fprintf(stderr,
                    "Usage: %s --cmd \"<program and args>\" [--runs N] "
                    "[--events e1,e2,e3] [--out file.csv]\n",
                    argv[0]);
            return 1;
        }
    }

    if (!cmd) {
        fprintf(stderr,
                "Usage: %s --cmd \"<program and args>\" [--runs N] "
                "[--events e1,e2,e3] [--out file.csv]\n",
                argv[0]);
        return 1;
    }

    /* Parse events_csv into individual event names */
    EventSamples events[MAX_EVENTS];
    int n_events = 0;
    char buf[MAX_LINE_LEN];
    strncpy(buf, events_csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* tok = strtok(buf, ",");
    while (tok && n_events < MAX_EVENTS) {
        event_samples_init(&events[n_events], tok, runs > 0 ? runs : 16);
        n_events++;
        tok = strtok(NULL, ",");
    }
    if (n_events == 0) {
        die("no events specified");
    }

#ifdef VERBOSE
    printf("Running '%s' %d times under perf stat...\n", cmd, runs);

    printf("Events:");
    for (int i = 0; i < n_events; i++) {
        printf(" %s", events[i].name);
    }
    printf("\n\n");
#endif

    int failures = 0;
    for (int i = 1; i <= runs; i++) {
        if (run_once(cmd, events_csv, events, n_events) != 0) {
            failures++;
        }
#ifdef VERBOSE
        if (i % 50 == 0 || i == runs) {
            printf("  completed %d/%d runs (%d failures so far)\n", i, runs,
                   failures);
        }
#endif
    }

    if (failures) {
        printf("\n%d/%d runs failed to produce valid counter values.\n",
               failures, runs);
    }

    FILE* csv_out = fopen("../results.csv", "a");
    for (int i = 0; i < n_events; i++) {
        EventSamples* es = &events[i];
        if (es->count == 0) {
            printf("%s: no valid samples collected\n", es->name);
            continue;
        }
        double mean, half_width;
        confidence_interval_95(es->values, es->count, &mean, &half_width);

#ifdef VERBOSE
        printf("\n=== Results (mean +/- 95%% CI half-width) ===\n");
        double sd = stdev_of(es->values, es->count, mean);

        printf("%s:\n", es->name);
        printf("  n            = %d\n", es->count);
        printf("  mean         = %.4f\n", mean);
        printf("  95%% CI       = [%.4f, %.4f]\n", mean - half_width,
               mean + half_width);
        if (es->count > 1) {
            printf("  stdev        = %.4f\n", sd);
        } else {
            printf("  stdev        = n/a\n");
        }
        printf("\n");
#endif

        if (csv_out) {
            fprintf(csv_out, ",%.4f,%.4f", mean, half_width);
        } else {
            fprintf(stderr,
                    "WARNING: could not open results.csv for appending\n");
            return EXIT_FAILURE;
        }
    }

    fprintf(csv_out, "\n");
    if (csv_out) {
        fclose(csv_out);
    }

    for (int i = 0; i < n_events; i++) {
        free(events[i].values);
    }

    return EXIT_SUCCESS;
}
