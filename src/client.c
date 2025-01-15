#include "common.h"
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <p101_fsm/fsm.h>
#include <p101_posix/p101_unistd.h>
#include <p101_posix/sys/p101_socket.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

static p101_fsm_state_t init_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t connect_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t process_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t cleanup_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t error_state(const struct p101_env *env, struct p101_error *err, void *arg);

enum client_states
{
    INIT = P101_FSM_USER_START,
    CONNECT,
    PROCESS,
    CLEANUP,
    ERROR,
};

struct client_context
{
    int                client_socket;
    struct sockaddr_un address;
};

int main(void)
{
    struct p101_error    *error;
    struct p101_env      *env;
    struct p101_error    *fsm_error;
    struct p101_env      *fsm_env;
    struct p101_fsm_info *fsm;

    error     = p101_error_create(false);
    env       = p101_env_create(error, true, NULL);
    fsm_error = p101_error_create(false);
    fsm_env   = p101_env_create(error, true, NULL);
    fsm       = p101_fsm_info_create(env, error, "client-fsm", fsm_env, fsm_error, NULL);

    if(p101_error_has_error(error))
    {
        fprintf(stderr, "Error creating FSM: %s\n", p101_error_get_message(error));
    }
    else
    {
        p101_fsm_state_t                  from_state;
        p101_fsm_state_t                  to_state;
        struct client_context             ctx           = {0};
        static struct p101_fsm_transition transitions[] = {
            {P101_FSM_INIT, INIT,          init_state   },
            {INIT,          CONNECT,       connect_state},
            {INIT,          ERROR,         error_state  },
            {CONNECT,       PROCESS,       process_state},
            {CONNECT,       ERROR,         error_state  },
            {PROCESS,       CLEANUP,       cleanup_state},
            {PROCESS,       ERROR,         error_state  },
            {CLEANUP,       P101_FSM_EXIT, NULL         },
            {ERROR,         P101_FSM_EXIT, NULL         },
        };

        from_state = P101_FSM_INIT;
        to_state   = INIT;
        p101_fsm_run(fsm, &from_state, &to_state, &ctx, transitions, sizeof(transitions) / sizeof(transitions[0]));
        p101_fsm_info_destroy(env, &fsm);
    }

    p101_error_reset(fsm_error);
    p101_free(env, fsm_error);
    p101_free(env, fsm_env);
    p101_error_reset(error);
    p101_free(env, error);
    free(env);

    return EXIT_SUCCESS;
}

static p101_fsm_state_t init_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    struct client_context *ctx;
    size_t                 sun_path_max_length;

    ctx                = (struct client_context *)arg;
    ctx->client_socket = p101_socket(env, err, AF_UNIX, SOCK_STREAM, 0);

    if(p101_error_has_error(err))
    {
        return ERROR;
    }

    memset(&ctx->address, 0, sizeof(ctx->address));
    ctx->address.sun_family = AF_UNIX;
    sun_path_max_length     = sizeof(ctx->address.sun_path);
    p101_strncpy(env, ctx->address.sun_path, socket_path, sun_path_max_length);

    return CONNECT;
}

static p101_fsm_state_t connect_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    struct client_context *ctx;

    ctx = (struct client_context *)arg;
    p101_connect(env, err, ctx->client_socket, (struct sockaddr *)&ctx->address, sizeof(ctx->address));

    if(p101_error_has_error(err))
    {
        return ERROR;
    }

    return PROCESS;
}

static p101_fsm_state_t process_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    const struct client_context *ctx;
    const char                  *message;
    char                        *buffer;
    ssize_t                      bytes_read;
    p101_fsm_state_t             next_state;

    ctx     = (struct client_context *)arg;
    buffer  = NULL;
    message = "Hello, World!!";
    p101_write(env, err, ctx->client_socket, message, strlen(message));

    if(p101_error_has_error(err))
    {
        next_state = ERROR;
        goto done;
    }

    buffer     = (char *)p101_malloc(env, err, buffer_size * sizeof(buffer[0]));
    bytes_read = p101_read(env, err, ctx->client_socket, buffer, buffer_size - 1);

    if(p101_error_has_error(err))
    {
        next_state = ERROR;
        goto done;
    }

    buffer[bytes_read] = '\0';
    printf("Received: %s\n", buffer);
    next_state = CLEANUP;
done:
    if(buffer != NULL)
    {
        p101_free(env, buffer);
    }

    return next_state;
}

static p101_fsm_state_t cleanup_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    const struct client_context *ctx;

    ctx = (struct client_context *)arg;
    p101_close(env, err, ctx->client_socket);

    return P101_FSM_EXIT;
}

static p101_fsm_state_t error_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    const struct client_context *ctx;
    const char                  *message;

    ctx = (struct client_context *)arg;

    if(ctx->client_socket != -1)
    {
        p101_close(env, err, ctx->client_socket);
    }

    message = p101_error_get_message(err);
    fprintf(stderr, "Error: %s, shutting down.\n", message);

    return P101_FSM_EXIT;
}
