#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <ctype.h>
#include <time.h>
#include <linux/unistd.h>
#include <sys/syscall.h>

#include <amd_smi/amdsmi.h>

#define HAVE_DEBUG
#define NUM_EVENTS 128 

#if !defined(BACKEND_SCOREP) && !defined(BACKEND_VTRACE)
#define BACKEND_VTRACE
#endif

#ifdef BACKEND_SCOREP
#include <scorep/SCOREP_MetricPlugins.h>
    typedef SCOREP_Metric_Plugin_MetricProperties metric_properties_t;
    typedef SCOREP_MetricTimeValuePair timevalue_t;
    typedef SCOREP_Metric_Plugin_Info plugin_info_type;
#endif

#ifdef BACKEND_VTRACE
#include <vampirtrace/vt_plugin_cntr.h>
    typedef vt_plugin_cntr_metric_info metric_properties_t;
    typedef vt_plugin_cntr_timevalue timevalue_t;
    typedef vt_plugin_cntr_info plugin_info_type;
#endif

typedef enum {
    AMDSMI_METRIC_ENERGY,
    AMDSMI_METRIC_POWER,
    AMDSMI_METRIC_TEMP,
    AMDSMI_METRIC_GFXCLK,
    AMDSMI_METRIC_BUSY,
    AMDSMI_METRIC_MEM_BUSY,
    AMDSMI_METRIC_ACC_PROCHOT_THRM_VIOLATION,
    AMDSMI_METRIC_ACC_PPT_PWR_VIOLATION,
    AMDSMI_METRIC_ACC_SOCKET_THRM_VIOLATION,
    AMDSMI_METRIC_ACC_VR_THRM_VIOLATION,
    AMDSMI_METRIC_ACC_HBM_THRM_VIOLATION,
    AMDSMI_METRIC_UNKNOWN,
} amdsmi_internal_metric_t;

struct event {
    int enabled;
    unsigned long cpu;
    int num_cntrs;
    amdsmi_internal_metric_t metric_types[NUM_EVENTS];
    int device_indices[NUM_EVENTS];
    uint64_t last_vals[NUM_EVENTS];
    pthread_t thread;
    long long *timevalues;
    int32_t sample_count;
}__attribute__((aligned(64)));

struct event event_list[512];
static int event_list_size = 0;
static volatile int counter_enabled = 1;
static pthread_mutex_t add_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int global_num_cntrs = 0;
static uint64_t (*wtime)(void) = NULL;

#define DEFAULT_BUF_SIZE (size_t)(16*1024*1024) 
static size_t buf_size = DEFAULT_BUF_SIZE; 
static int interval_us = 1000; 

static amdsmi_processor_handle all_handles[64];
static int num_all_handles = 0;

static amdsmi_violation_status_t cached_violations[64];
static pthread_t violation_thread;
static int violation_poller_running = 0;

static pthread_mutex_t lock;

static size_t parse_buffer_size(const char *s) {
    char *tmp = NULL;
    size_t size = strtoll(s, &tmp, 10);
    if (size == 0) return DEFAULT_BUF_SIZE;
    while(*tmp == ' ') tmp++;
    switch(*tmp) { case 'G': size*=1024; case 'M': size*=1024; case 'K': size*=1024; }
    return size;
}

void set_pform_wtime_function(uint64_t(*pform_wtime)(void)) { wtime = pform_wtime; }

int32_t init_amd_smi(void)
{
    char * env_string;
    pthread_mutex_init(&lock, NULL);

#if defined(BACKEND_SCOREP)
    env_string = getenv("SCOREP_METRIC_AMD_SMI_INTERVAL_US");
#endif
    if (env_string != NULL) {
        interval_us = atoi(env_string);
        if (interval_us == 0) interval_us = 1000;
    }

#if defined(BACKEND_SCOREP)
    env_string = getenv("SCOREP_METRIC_AMD_SMI_BUF_SIZE");
#endif
    if (env_string != NULL) buf_size = parse_buffer_size(env_string);

    if (amdsmi_init(AMDSMI_INIT_AMD_GPUS) != AMDSMI_STATUS_SUCCESS) return -1;

    uint32_t socket_count = 0;
    amdsmi_get_socket_handles(&socket_count, NULL);
    if (socket_count == 0) return -1;
    
    amdsmi_socket_handle *sockets = malloc(socket_count * sizeof(amdsmi_socket_handle));
    amdsmi_get_socket_handles(&socket_count, sockets);

    num_all_handles = 0;
    for (uint32_t s = 0; s < socket_count; s++) {
        uint32_t p_count = 0;
        amdsmi_get_processor_handles(sockets[s], &p_count, NULL);
        if (p_count > 0) {
            amdsmi_processor_handle *procs = malloc(p_count * sizeof(amdsmi_processor_handle));
            amdsmi_get_processor_handles(sockets[s], &p_count, procs);
            
            for (uint32_t i = 0; i < p_count && num_all_handles < 64; i++) {
                processor_type_t ptype;
                if (amdsmi_get_processor_type(procs[i], &ptype) == AMDSMI_STATUS_SUCCESS) {
                    if (ptype == AMDSMI_PROCESSOR_TYPE_AMD_GPU) {
                        all_handles[num_all_handles] = procs[i];
                        memset(&cached_violations[num_all_handles], 0, sizeof(amdsmi_violation_status_t));
                        num_all_handles++;
                    }
                }
            }
            free(procs);
        }
    }
    free(sockets);
    return 0;
}

metric_properties_t * get_event_info(char * event_name)
{
    if (global_num_cntrs >= NUM_EVENTS) return NULL;
    metric_properties_t *return_values = malloc(2 * sizeof(metric_properties_t));
    if (return_values == NULL) return NULL;

    #define STR_SIZE 256
    char apapi_name[STR_SIZE];
    memset(apapi_name, 0, STR_SIZE);
    strcpy(apapi_name, "A2");
    strncat(apapi_name, event_name, STR_SIZE - 4);

    return_values[0].name = strdup(apapi_name);
    return_values[0].unit = NULL;
#ifdef BACKEND_SCOREP
    return_values[ 0 ].description = "AMD SMI Direct API Metric";
    return_values[ 0 ].mode        = SCOREP_METRIC_MODE_ABSOLUTE_LAST; 
    return_values[ 0 ].value_type  = SCOREP_METRIC_VALUE_UINT64;
    return_values[ 0 ].base        = SCOREP_METRIC_BASE_DECIMAL;
    return_values[ 0 ].exponent    = 0;
#endif
    return_values[1].name = NULL;
    global_num_cntrs++;
    return return_values;
}

void * violation_poller_thread_func(void * arg)
{
    while (counter_enabled) {
        for (int i = 0; i < num_all_handles; i++) {
            pthread_mutex_lock(&lock);
            amdsmi_gpu_metrics_t metrics;
            memset(&metrics, 0, sizeof(metrics));
            
            if (amdsmi_get_gpu_metrics_info(all_handles[i], &metrics) == AMDSMI_STATUS_SUCCESS) {
                cached_violations[i].acc_hbm_thrm     = metrics.hbm_thm_residency_acc;
                cached_violations[i].acc_ppt_pwr      = metrics.ppt_residency_acc;
                cached_violations[i].acc_prochot_thrm = metrics.prochot_residency_acc;
                cached_violations[i].acc_socket_thrm  = metrics.socket_thm_residency_acc;
                cached_violations[i].acc_vr_thrm      = metrics.vr_thm_residency_acc;
            }
            pthread_mutex_unlock(&lock);
        }
        usleep(1000);
    }
    return NULL;
}

void fini(void) { amdsmi_shut_down(); }

void * thread_report(void * _id)
{
    int id = (intptr_t) _id;
    struct event *ev = &event_list[id];

    size_t num_buf_elems = buf_size / sizeof(long long);
    ev->timevalues = calloc(num_buf_elems, sizeof(long long));
    size_t tv_pos = 0;
    ev->sample_count = 0;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset); 
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    struct timespec next_wakeup;
    clock_gettime(CLOCK_MONOTONIC, &next_wakeup);

    while (counter_enabled) {
        if (wtime == NULL) break;

        if (ev->enabled) {
            if ((tv_pos + ev->num_cntrs + 1) > num_buf_elems) break;
            ev->timevalues[tv_pos++] = wtime();

            for (int i = 0; i < ev->num_cntrs; i++) {
                int dev = ev->device_indices[i];
                uint64_t current_val = ev->last_vals[i];
                
                if (dev >= 0 && dev < num_all_handles) {
                    amdsmi_processor_handle ph = all_handles[dev];
                    pthread_mutex_lock(&lock);
                    switch(ev->metric_types[i]) {
                        case AMDSMI_METRIC_ENERGY: {
                            uint64_t energy = 0, ts = 0; float res = 0;
                            if (amdsmi_get_energy_count(ph, &energy, &res, &ts) == AMDSMI_STATUS_SUCCESS) {
                                uint64_t candidate_val = (uint64_t)((double)energy * (double)res);
                                
                                if (candidate_val >= ev->last_vals[i]) {
                                    current_val = candidate_val;
                                } else {
                                    current_val = ev->last_vals[i];
                                }
                            }
                            break;
                        }
                        case AMDSMI_METRIC_POWER: {
                            amdsmi_power_info_t pinfo;
                            if (amdsmi_get_power_info(ph, &pinfo) == AMDSMI_STATUS_SUCCESS)
                                current_val = pinfo.average_socket_power;
                            break;
                        }
                        case AMDSMI_METRIC_TEMP: {
                            int64_t temp = 0;
                            if (amdsmi_get_temp_metric(ph, AMDSMI_TEMPERATURE_TYPE_HOTSPOT, AMDSMI_TEMP_CURRENT, &temp) == AMDSMI_STATUS_SUCCESS)
                                current_val = temp;
                            break;
                        }
                        case AMDSMI_METRIC_GFXCLK: {
                            amdsmi_clk_info_t clk;
                            if (amdsmi_get_clock_info(ph, AMDSMI_CLK_TYPE_GFX, &clk) == AMDSMI_STATUS_SUCCESS)
                                current_val = clk.clk;
                            break;
                        }
                        case AMDSMI_METRIC_BUSY: {
                            amdsmi_engine_usage_t usage;
                            if (amdsmi_get_gpu_activity(ph, &usage) == AMDSMI_STATUS_SUCCESS)
                                current_val = usage.gfx_activity;
                            break;
                        }
                        case AMDSMI_METRIC_MEM_BUSY: {
                            amdsmi_engine_usage_t usage;
                            if (amdsmi_get_gpu_activity(ph, &usage) == AMDSMI_STATUS_SUCCESS)
                                current_val = usage.umc_activity;
                            break;
                        }
                        /* --- Lecturas asíncronas desde el caché --- */
                        case AMDSMI_METRIC_ACC_HBM_THRM_VIOLATION:
                            current_val = cached_violations[dev].acc_hbm_thrm; break;
                        case AMDSMI_METRIC_ACC_PPT_PWR_VIOLATION:
                            current_val = cached_violations[dev].acc_ppt_pwr; break;
                        case AMDSMI_METRIC_ACC_PROCHOT_THRM_VIOLATION:
                            current_val = cached_violations[dev].acc_prochot_thrm; break;
                        case AMDSMI_METRIC_ACC_SOCKET_THRM_VIOLATION:
                            current_val = cached_violations[dev].acc_socket_thrm; break;
                        case AMDSMI_METRIC_ACC_VR_THRM_VIOLATION:
                            current_val = cached_violations[dev].acc_vr_thrm; break;
                        default:
                            current_val = 0;
                            break;
                    }
                    pthread_mutex_unlock(&lock);
                }
                
                ev->last_vals[i] = current_val;
                ev->timevalues[tv_pos++] = current_val;
            }
            ev->sample_count++;
        }

        next_wakeup.tv_nsec += (interval_us * 1000); 
        while (next_wakeup.tv_nsec >= 1000000000) {
            next_wakeup.tv_nsec -= 1000000000;
            next_wakeup.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);
    }
    return NULL;
}

int32_t add_counter(char * event_name)
{
    int i, id = -1;
    pthread_mutex_lock(&add_counter_mutex);
    unsigned long cpu = syscall(__NR_gettid);
    
    for (i=0; i<event_list_size; i++) {
        if (event_list[i].cpu == cpu) { id = i; break; }
    }

    if (id == -1) {
        id = event_list_size++;
        memset(&event_list[id], 0, sizeof(struct event));
        event_list[id].cpu = cpu;
        event_list[id].enabled = 1;
        event_list[id].num_cntrs = 0;
    }

    int idx = event_list[id].num_cntrs;
    int counter_id = (id << 8) + idx;

    amdsmi_internal_metric_t type = AMDSMI_METRIC_UNKNOWN;
    
    if (strstr(event_name, "energy")) type = AMDSMI_METRIC_ENERGY;
    else if (strstr(event_name, "power")) type = AMDSMI_METRIC_POWER;
    else if (strstr(event_name, "temp")) type = AMDSMI_METRIC_TEMP;
    else if (strstr(event_name, "clk")) type = AMDSMI_METRIC_GFXCLK;
    else if (strstr(event_name, "hbm_thrm_violation_acc")) type = AMDSMI_METRIC_ACC_HBM_THRM_VIOLATION;
    else if (strstr(event_name, "ppt_pwr_violation_acc")) type = AMDSMI_METRIC_ACC_PPT_PWR_VIOLATION;
    else if (strstr(event_name, "prochot_thrm_violation_acc")) type = AMDSMI_METRIC_ACC_PROCHOT_THRM_VIOLATION;
    else if (strstr(event_name, "socket_thrm_violation_acc")) type = AMDSMI_METRIC_ACC_SOCKET_THRM_VIOLATION;
    else if (strstr(event_name, "vr_thrm_violation_acc")) type = AMDSMI_METRIC_ACC_VR_THRM_VIOLATION;
    else if (strstr(event_name, "memory") || strstr(event_name, "umc")) type = AMDSMI_METRIC_MEM_BUSY;
    else if (strstr(event_name, "busy") || strstr(event_name, "gfx")) type = AMDSMI_METRIC_BUSY;

    if (strstr(event_name, "violation") && !violation_poller_running) {
        violation_poller_running = 1;
        pthread_create(&violation_thread, NULL, &violation_poller_thread_func, NULL);
    }

    int dev_idx = 0;
    char *dev_ptr = strstr(event_name, "device=");
    if (dev_ptr) dev_idx = atoi(dev_ptr + 7);

    event_list[id].metric_types[idx] = type;
    event_list[id].device_indices[idx] = dev_idx;
    event_list[id].last_vals[idx] = 0;
    event_list[id].num_cntrs++;

    pthread_mutex_unlock(&add_counter_mutex);

    if (event_list[id].num_cntrs == global_num_cntrs) {
        pthread_create(&event_list[id].thread, NULL, &thread_report, (void *)(intptr_t) id);
    }
    return counter_id;
}

uint64_t get_all_values(int32_t id, timevalue_t **result)
{
    int evt_id = id >> 8;
    int cntr_id = id & 0xff;

    if (counter_enabled) {
        counter_enabled = 0;
        for (int i=0; i<event_list_size; i++) pthread_join(event_list[i].thread, NULL);
        if (violation_poller_running) {
            pthread_join(violation_thread, NULL);
            violation_poller_running = 0;
        }
    }

    timevalue_t *res = malloc(event_list[evt_id].sample_count * sizeof(timevalue_t));
    if (res == NULL) return 0;
    
    long long *timevalues = event_list[evt_id].timevalues;
    size_t tv_pos = 0;
    
    for (int i = 0; i < event_list[evt_id].sample_count; i++) {
        res[i].timestamp = timevalues[tv_pos++];
        res[i].value = timevalues[tv_pos + cntr_id];
        tv_pos += event_list[evt_id].num_cntrs;
    }
    
    *result = res;
    return event_list[evt_id].sample_count;
}

int enable_counter(int ID) { event_list[ID].enabled = 1; return 0; }
int disable_counter(int ID) { event_list[ID].enabled = 0; return 0; }

#ifdef BACKEND_SCOREP
SCOREP_METRIC_PLUGIN_ENTRY( amd_smi_plugin )
{
    plugin_info_type info;
    memset(&info,0,sizeof(plugin_info_type));
    info.plugin_version               = SCOREP_METRIC_PLUGIN_VERSION;
    info.run_per                      = SCOREP_METRIC_PER_HOST; 
    info.sync                         = SCOREP_METRIC_ASYNC;
    info.delta_t                      = UINT64_MAX;
    info.initialize                   = init_amd_smi;
    info.set_clock_function           = set_pform_wtime_function;
    info.add_counter                  = add_counter;
    info.get_event_info               = get_event_info;
    info.get_all_values               = get_all_values;
    info.finalize                     = fini;
    return info;
}
#endif