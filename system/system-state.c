#include "qemu/osdep.h"
#include "system-state.h"
#include "qemu/timer.h"
#include "qemu/log.h"

static struct state_thresholds thresholds;
static int current_state = STATE_UNKNOWN;
static int current_mode = MODE_DISABLED;
static int training_state = STATE_UNKNOWN;
static state_change_cb state_callback;
static void *callback_data;

/* Training data for each state */
static struct state_training_data training_data[3]; // PROM_IDLE, OS_IDLE, SHUTDOWN

void system_state_init(void)
{
    memcpy(&thresholds, &default_thresholds, sizeof(thresholds));
    current_state = STATE_UNKNOWN;
    current_mode = MODE_DISABLED;
    training_state = STATE_UNKNOWN;
    state_callback = NULL;
    callback_data = NULL;
    for (int i = 0; i < 4; i++) {
        memset(&training_data[i], 0, sizeof(struct state_training_data));
        training_data[i].func_stats = g_malloc0(sizeof(*training_data[i].func_stats) * MAX_FUNCTIONS);
    }
}

void system_state_set_mode(int mode)
{
    if (mode >= MODE_TRAINING && mode <= MODE_DISABLED) {
        if (mode == MODE_TRAINING) {
            /* Reset training data when entering training mode */
            for (int i = 0; i < 4; i++) {
                memset(&training_data[i], 0, sizeof(struct state_training_data));
                training_data[i].func_stats = g_malloc0(sizeof(*training_data[i].func_stats) * MAX_FUNCTIONS);
            }
        }
        current_mode = mode;
        qemu_log("System state mode changed to: %s\n",
                mode == MODE_TRAINING ? "training" :
                mode == MODE_DETECTION ? "detection" : "disabled");
    }
}

int system_state_get_mode(void)
{
    return current_mode;
}

void system_state_set_training_state(int state)
{
    if (current_mode != MODE_TRAINING) {
        qemu_log("Warning: Cannot set training state when not in training mode\n");
        return;
    }

    if (state >= STATE_UNKNOWN && state <= STATE_SHUTDOWN) {
        training_state = state;
        qemu_log("Training state set to: %d\n", state);
    }
}

static void update_function_stats(struct state_training_data *data,
                                const struct debug_counters *counters)
{
    /* Find or create function stats */
    int func_idx = -1;
    for (int i = 0; i < data->num_funcs; i++) {
        if (data->func_stats[i].func_id == counters->func_id) {
            func_idx = i;
            break;
        }
    }

    if (func_idx == -1 && data->num_funcs < MAX_FUNCTIONS) {
        func_idx = data->num_funcs++;
        data->func_stats[func_idx].func_id = counters->func_id;
        /* Initialize ranges with first sample */
        data->func_stats[func_idx].min_true_ns = counters->min_true_ns;
        data->func_stats[func_idx].max_true_ns = counters->min_true_ns;
        data->func_stats[func_idx].min_false_ns = counters->min_false_ns;
        data->func_stats[func_idx].max_false_ns = counters->min_false_ns;
        data->func_stats[func_idx].min_true_streak = counters->current_true_streak;
        data->func_stats[func_idx].max_true_streak = counters->current_true_streak;
        data->func_stats[func_idx].min_false_streak = counters->current_false_streak;
        data->func_stats[func_idx].max_false_streak = counters->current_false_streak;
    }

    if (func_idx != -1) {
        struct func_stats *func = &data->func_stats[func_idx];

        /* Update stats with current sample */
        func->true_count = counters->true_count;
        func->false_count = counters->false_count;

        /* Update timing ranges */
        if (counters->min_true_ns < func->min_true_ns) {
            func->min_true_ns = counters->min_true_ns;
        }
        if (counters->min_true_ns > func->max_true_ns) {
            func->max_true_ns = counters->min_true_ns;
        }
        if (counters->min_false_ns < func->min_false_ns) {
            func->min_false_ns = counters->min_false_ns;
        }
        if (counters->min_false_ns > func->max_false_ns) {
            func->max_false_ns = counters->min_false_ns;
        }

        /* Update streak ranges */
        if (counters->current_true_streak < func->min_true_streak) {
            func->min_true_streak = counters->current_true_streak;
        }
        if (counters->current_true_streak > func->max_true_streak) {
            func->max_true_streak = counters->current_true_streak;
        }
        if (counters->current_false_streak < func->min_false_streak) {
            func->min_false_streak = counters->current_false_streak;
        }
        if (counters->current_false_streak > func->max_false_streak) {
            func->max_false_streak = counters->current_false_streak;
        }

        /* Update confidence scores */
        if (func->true_count > 0) {
            /* How well do current timings match the learned pattern? */
            float timing_diff = 0;
            if (func->max_true_ns > func->min_true_ns) {
                timing_diff = (float)(counters->min_true_ns - func->min_true_ns) /
                            (func->max_true_ns - func->min_true_ns);
                timing_diff = timing_diff > 1 ? 1 : (timing_diff < 0 ? 0 : timing_diff);
                func->confidence.timing_match = 1.0f - timing_diff;
            }

            /* How well do current streaks match the learned pattern? */
            float streak_diff = 0;
            if (func->max_true_streak > func->min_true_streak) {
                streak_diff = (float)(counters->current_true_streak - func->min_true_streak) /
                            (func->max_true_streak - func->min_true_streak);
                streak_diff = streak_diff > 1 ? 1 : (streak_diff < 0 ? 0 : streak_diff);
                func->confidence.streak_match = 1.0f - streak_diff;
            }

            /* Combine confidence scores */
            func->confidence.overall = (func->confidence.timing_match + func->confidence.streak_match) / 2;
        }
    }
}

static void update_state_confidence(struct state_training_data *data)
{
    if (data->num_funcs == 0) {
        data->overall_confidence = 0;
        return;
    }

    float total_confidence = 0;
    for (int i = 0; i < data->num_funcs; i++) {
        total_confidence += data->func_stats[i].confidence.overall;
    }
    data->overall_confidence = total_confidence / data->num_funcs;
}

int system_state_load_config(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        return -1;
    }

    /* Read training data from file */
    for (int state = 0; state < 4; state++) {
        struct state_training_data *data = &training_data[state];
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "state_%d_", state);

        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, prefix, strlen(prefix)) != 0) {
                continue;
            }

            char key[128];
            unsigned long long value;
            if (sscanf(line, "%127[^=]=%llu", key, &value) != 2) {
                continue;
            }

            // TODO from the file, read min and max and true and false for each of:-
            // count, min_streak, max_streak, min_ns, max_ns, avg_ns (these are the 6 initial variables)
            // i.e.:-
            // count: min_true_count, max_true_count, min_false_count, max_false_count
            // min_streak: min_min_true_streak, max_min_true_streak, min_max_true_streak, max_max_true_streak
            // max_streak: min_min_false_streak, max_min_false_streak, min_max_false_streak, max_max_false_streak
            // min_ns: min_min_true_ns, max_min_true_ns, min_max_true_ns, max_max_true_ns
            // max_ns: min_min_false_ns, max_min_false_ns, min_max_false_ns, max_max_false_ns
            // avg_ns: min_avg_true_ns, max_avg_true_ns, min_avg_false_ns, max_avg_false_ns
            //
            // but do it in a way that is concise and just uses the 6 initial variables
        }

        rewind(f);
    }

    fclose(f);
    return 0;
}

int system_state_save_config(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        return -1;
    }

    /* Save thresholds derived from training data */
    // TODO - save the 6 initial variables for each state - expand to min/max, true/false.

    fclose(f);
    return 0;
}

static void update_state(int new_state)
{
    if (new_state != current_state) {
        if (state_callback) {
            state_callback(current_state, new_state, callback_data);
        }
        current_state = new_state;
    }
}

int system_state_update(const struct debug_counters *counters)
{
    if (!counters) {
        return -1;
    }

    if (current_mode == MODE_TRAINING) {
        update_training_data(counters);
        return training_state;
    }

    if (current_mode != MODE_DETECTION) {
        return STATE_UNKNOWN;
    }

    /* Use trained thresholds for detection */
    for (int state = 1; state <= STATE_SHUTDOWN; state++) {
        const struct state_training_data *data = &training_data[state];
        if (data->samples == 0) {
            continue;  /* Skip states with no training data */
        }

        /* Check if current metrics match this state's profile */
        if (counters->true_count >= data->true_count_min &&
            counters->true_count <= data->true_count_max &&
            counters->false_count >= data->false_count_min &&
            counters->false_count <= data->false_count_max &&
            counters->min_true_ns >= data->min_interval_ns &&
            counters->max_true_ns <= data->max_interval_ns &&
            counters->current_true_streak >= data->min_streak &&
            counters->current_true_streak <= data->max_streak) {
            update_state(state);
            return state;
        }
    }

    update_state(STATE_UNKNOWN);
    return STATE_UNKNOWN;
}

int system_state_get(void)
{
    return current_state;
}

void system_state_register_callback(state_change_cb cb, void *data)
{
    state_callback = cb;
    callback_data = data;
}

const struct state_training_data *system_state_get_training_data(int state)
{
    if (state >= STATE_UNKNOWN && state <= STATE_SHUTDOWN) {
        return &training_data[state];
    }
    return NULL;
}
