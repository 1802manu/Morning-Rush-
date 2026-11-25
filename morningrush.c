#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define MAX_QUEUE 2048
#define MAX_SERVERS 8
#define HISTORY_LEN 60

typedef struct {
    int id;
    double arrived_at;
    double patience;
} Customer;

typedef struct {
    bool active;
    Customer customer;
    double remaining;
    double total;
} Server;

typedef struct {
    double t;
    Customer queue[MAX_QUEUE];
    int q_count;
    Server servers[MAX_SERVERS];
    int num_servers;
    int served;
    int left;
    double wait_sum;
    double busy_time;
    double total_server_time;
    int next_id;
    int history[HISTORY_LEN];
    int hist_count;
} Simulation;

static double rand_uniform(void) {
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

static double rand_expo(double mean) {
    double u = rand_uniform();
    if (u >= 0.999999) u = 0.999999;
    return -mean * log(1.0 - u);
}

static void sim_init(Simulation *sim, int num_servers) {
    memset(sim, 0, sizeof(*sim));
    if (num_servers < 1) num_servers = 1;
    if (num_servers > MAX_SERVERS) num_servers = MAX_SERVERS;
    sim->num_servers = num_servers;
    sim->next_id = 1;
}

static void sim_push_queue(Simulation *sim, Customer c) {
    if (sim->q_count < MAX_QUEUE) {
        sim->queue[sim->q_count++] = c;
    }
}

static Customer sim_pop_queue(Simulation *sim) {
    Customer front = sim->queue[0];
    if (sim->q_count > 1) {
        memmove(&sim->queue[0], &sim->queue[1], sizeof(Customer) * (size_t)(sim->q_count - 1));
    }
    sim->q_count--;
    return front;
}

static void sim_remove_queue_at(Simulation *sim, int index) {
    if (index < 0 || index >= sim->q_count) return;
    if (index < sim->q_count - 1) {
        memmove(&sim->queue[index], &sim->queue[index + 1], sizeof(Customer) * (size_t)(sim->q_count - index - 1));
    }
    sim->q_count--;
}

static void sim_step(Simulation *sim, double dt, double arrival_per_min, bool rush_active) {
    double lambda = (arrival_per_min * (rush_active ? 2.0 : 1.0)) / 60.0;

    /* Poisson arrival check */
    if (rand_uniform() < (1.0 - exp(-lambda * dt))) {
        Customer c;
        c.id = sim->next_id++;
        c.arrived_at = sim->t;
        c.patience = 240.0 + rand_uniform() * 240.0;
        sim_push_queue(sim, c);
    }

    /* Check customer impatience / walkouts */
    for (int i = sim->q_count - 1; i >= 0; i--) {
        if ((sim->t - sim->queue[i].arrived_at) > sim->queue[i].patience) {
            sim_remove_queue_at(sim, i);
            sim->left++;
        }
    }

    /* Update baristas and assign waiting customers */
    for (int i = 0; i < sim->num_servers; i++) {
        Server *s = &sim->servers[i];
        if (s->active) {
            s->remaining -= dt;
            sim->busy_time += dt;
            if (s->remaining <= 0.0) {
                sim->served++;
                s->active = false;
            }
        }
        if (!s->active && sim->q_count > 0) {
            Customer c = sim_pop_queue(sim);
            double service_time = rand_expo(75.0);
            if (service_time < 20.0) service_time = 20.0;
            s->active = true;
            s->customer = c;
            s->total = service_time;
            s->remaining = service_time;
            sim->wait_sum += (sim->t - c.arrived_at);
        }
    }

    sim->total_server_time += dt * sim->num_servers;
    sim->t += dt;

    int bucket = (int)(sim->t / 6.0);
    if (bucket > sim->hist_count) {
        if (sim->hist_count < HISTORY_LEN) {
            sim->history[sim->hist_count++] = sim->q_count;
        } else {
            memmove(&sim->history[0], &sim->history[1], sizeof(int) * (HISTORY_LEN - 1));
            sim->history[HISTORY_LEN - 1] = sim->q_count;
        }
    }
}

static void format_time(double seconds, char *buf, size_t buf_size) {
    int total = (int)llround(seconds);
    int mins = total / 60;
    int secs = total % 60;
    snprintf(buf, buf_size, "%d:%02d", mins, secs);
}

static void render_tui(const Simulation *sim, double arrival_per_min, bool rush_active, double rush_left) {
    int active_servers = 0;
    for (int i = 0; i < sim->num_servers; i++) {
        if (sim->servers[i].active) active_servers++;
    }

    double avg_wait = 0.0;
    if (sim->served > 0) {
        int denom = sim->served + active_servers;
        if (denom < 1) denom = 1;
        avg_wait = sim->wait_sum / (double)denom;
    }

    int busy_pct = sim->total_server_time > 0.0
        ? (int)llround(100.0 * sim->busy_time / sim->total_server_time)
        : 0;

    char sim_clock[32];
    char wait_str[32];
    format_time(sim->t, sim_clock, sizeof(sim_clock));
    format_time(avg_wait, wait_str, sizeof(wait_str));

    /* Clear screen and move cursor to home */
    printf("\033[2J\033[H");
    printf("============================================================\n");
    printf("  MORNING RUSH AT THE COUNTER (C Live Queue Simulation)\n");
    printf("============================================================\n");
    printf("  Sim Time: %-8s | Baristas: %d | Arrival Rate: %.1f/min\n",
           sim_clock, sim->num_servers, arrival_per_min);
    if (rush_active) {
        printf("  Status  : [RUSH ACTIVE - %.0fs remaining (2x arrivals)]\n", rush_left);
    } else {
        printf("  Status  : Normal traffic\n");
    }
    printf("------------------------------------------------------------\n");
    printf("  Waiting: %-4d | Avg Wait: %-6s | Served: %-4d | Walked Out: %-4d | Busy: %d%%\n",
           sim->q_count, wait_str, sim->served, sim->left, busy_pct);
    printf("------------------------------------------------------------\n\n");

    /* Queue visualization */
    printf("  Queue (front on right): ");
    int shown = sim->q_count > 30 ? 30 : sim->q_count;
    if (sim->q_count > 30) {
        printf("[+%d] ", sim->q_count - 30);
    }
    for (int i = shown - 1; i >= 0; i--) {
        double wait_ratio = (sim->t - sim->queue[i].arrived_at) / sim->queue[i].patience;
        putchar(wait_ratio > 0.66 ? '!' : 'o');
    }
    if (sim->q_count == 0) {
        printf("(empty)");
    }
    printf("\n  Legend: 'o' = waiting, '!' = losing patience (>66%%)\n\n");

    /* Barista counters */
    printf("  Baristas:\n");
    for (int i = 0; i < sim->num_servers; i++) {
        const Server *s = &sim->servers[i];
        printf("    Counter %d: [", i + 1);
        int bar_width = 24;
        if (s->active) {
            double frac = 1.0 - (s->remaining / s->total);
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            int filled = (int)(frac * bar_width);
            for (int b = 0; b < bar_width; b++) {
                putchar(b < filled ? '#' : '.');
            }
            printf("] Order #%d (%.0fs left)\n", s->customer.id, ceil(s->remaining));
        } else {
            for (int b = 0; b < bar_width; b++) putchar(' ');
            printf("] idle\n");
        }
    }

    /* Recent queue length sparkline */
    printf("\n  Queue History (last %d samples):\n  ", sim->hist_count);
    const char *bars = " .:-=+*#%@";
    int max_q = 10;
    for (int i = 0; i < sim->hist_count; i++) {
        if (sim->history[i] > max_q) max_q = sim->history[i];
    }
    for (int i = 0; i < sim->hist_count; i++) {
        int idx = (sim->history[i] * 8) / max_q;
        if (idx < 0) idx = 0;
        if (idx > 8) idx = 8;
        putchar(bars[idx]);
    }
    printf("\n\n  Press Ctrl+C to stop.\n");
    fflush(stdout);
}

int main(void) {
    srand((unsigned int)time(NULL));

    int num_baristas = 3;
    double arrival_rate = 2.5;
    int speed = 2;
    int trigger_rush = 1;

    printf("Morning Rush: Live Queue Simulation (C Port)\n");
    printf("Baristas on shift [1-8, default 3]: ");
    if (scanf("%d", &num_baristas) != 1 || num_baristas < 1 || num_baristas > 8) {
        num_baristas = 3;
    }

    printf("Customers arriving per minute [0.5-9.0, default 2.5]: ");
    if (scanf("%lf", &arrival_rate) != 1 || arrival_rate < 0.5 || arrival_rate > 9.0) {
        arrival_rate = 2.5;
    }

    printf("Simulation speed [1-5, default 2]: ");
    if (scanf("%d", &speed) != 1 || speed < 1 || speed > 5) {
        speed = 2;
    }

    printf("Trigger 2-minute rush at t=60s? (1=Yes, 0=No, default 1): ");
    if (scanf("%d", &trigger_rush) != 1) {
        trigger_rush = 1;
    }

    Simulation sim;
    sim_init(&sim, num_baristas);

    double rush_left = 0.0;
    bool rush_triggered = false;
    const double real_dt = 0.1; /* 10 FPS terminal refresh */

    while (1) {
        double sim_dt = real_dt * 10.0 * (double)speed;
        int substeps = (int)ceil(sim_dt);
        if (substeps < 1) substeps = 1;

        for (int k = 0; k < substeps; k++) {
            double step_dt = sim_dt / (double)substeps;
            if (trigger_rush && !rush_triggered && sim.t >= 60.0) {
                rush_triggered = true;
                rush_left = 120.0;
            }
            if (rush_left > 0.0) {
                rush_left -= step_dt;
                if (rush_left < 0.0) rush_left = 0.0;
            }
            sim_step(&sim, step_dt, arrival_rate, rush_left > 0.0);
        }

        render_tui(&sim, arrival_rate, rush_left > 0.0, rush_left);
        SLEEP_MS(100);
    }

    return 0;
}
