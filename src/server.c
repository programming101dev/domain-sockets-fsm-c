#include "common.h"
#include <p101_c/p101_signal.h>
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <p101_fsm/fsm.h>
#include <p101_posix/p101_unistd.h>
#include <p101_posix/sys/p101_socket.h>
#include <signal.h>
#include <stdio.h>
#include <sys/un.h>

static void             handle_sigint(int signal);
static p101_fsm_state_t init_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t bind_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t listen_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t accept_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t handle_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t cleanup_state(const struct p101_env *env, struct p101_error *err, void *arg);
static p101_fsm_state_t error_state(const struct p101_env *env, struct p101_error *err, void *arg);

enum server_states
{
    INIT = P101_FSM_USER_START,
    BIND,
    LISTEN,
    ACCEPT,
    HANDLE,
    CLEANUP,
    ERROR,
};

struct server_context
{
    int                server_socket;
    int                client_socket;
    struct sockaddr_un address;
    int                backlog;
};

static const int backlog = 5;

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
    fsm       = p101_fsm_info_create(env, error, "server-fsm", fsm_env, fsm_error, NULL);

    if(p101_error_has_error(error))
    {
        fprintf(stderr, "Error creating FSM: %s\n", p101_error_get_message(error));
    }
    else
    {
        p101_fsm_state_t                  from_state;
        p101_fsm_state_t                  to_state;
        struct server_context             ctx           = {0};
        static struct p101_fsm_transition transitions[] = {
            {P101_FSM_INIT, INIT,          init_state   },
            {INIT,          BIND,          bind_state   },
            {INIT,          ERROR,         error_state  },
            {BIND,          LISTEN,        listen_state },
            {BIND,          ERROR,         error_state  },
            {LISTEN,        ACCEPT,        accept_state },
            {LISTEN,        ERROR,         error_state  },
            {ACCEPT,        HANDLE,        handle_state },
            {ACCEPT,        ERROR,         error_state  },
            {HANDLE,        CLEANUP,       cleanup_state},
            {HANDLE,        ERROR,         error_state  },
            {CLEANUP,       ACCEPT,        accept_state },
            {CLEANUP,       ERROR,         error_state  },
            {ERROR,         P101_FSM_EXIT, NULL         },
        };

        from_state  = P101_FSM_INIT;
        to_state    = INIT;
        ctx.backlog = backlog;
        p101_signal(env, error, SIGINT, handle_sigint);
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void handle_sigint(int signal)
{
}

#pragma GCC diagnostic pop

static p101_fsm_state_t init_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    struct server_context *ctx;
    size_t                 sun_path_max_length;

    ctx                = (struct server_context *)arg;
    ctx->server_socket = p101_socket(env, err, AF_UNIX, SOCK_STREAM, 0);

    if(ctx->server_socket == -1)
    {
        perror("socket");
        return ERROR;
    }

    memset(&ctx->address, 0, sizeof(ctx->address));
    ctx->address.sun_family = AF_UNIX;
    sun_path_max_length     = sizeof(ctx->address.sun_path);
    p101_strncpy(env, ctx->address.sun_path, socket_path, sun_path_max_length);

    return BIND;
}

static p101_fsm_state_t bind_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    struct server_context *ctx;

    ctx = (struct server_context *)arg;
    p101_bind(env, err, ctx->server_socket, (struct sockaddr *)&ctx->address, sizeof(ctx->address));

    if(p101_error_has_error(err))
    {
        return ERROR;
    }

    return LISTEN;
}

static p101_fsm_state_t listen_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    const struct server_context *ctx;

    ctx = (struct server_context *)arg;

    p101_listen(env, err, ctx->server_socket, ctx->backlog);

    if(p101_error_has_error(err))
    {
        return ERROR;
    }

    return ACCEPT;
}

static p101_fsm_state_t accept_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    struct server_context *ctx;

    ctx                = (struct server_context *)arg;
    ctx->client_socket = p101_accept(env, err, ctx->server_socket, NULL, NULL);

    if(p101_error_has_error(err))
    {
        return ERROR;
    }

    return HANDLE;
}

static p101_fsm_state_t handle_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    const struct server_context *ctx;
    char                        *buffer;
    ssize_t                      bytes_read;
    p101_fsm_state_t             next_state;

    ctx        = (struct server_context *)arg;
    buffer     = (char *)p101_malloc(env, err, buffer_size * sizeof(buffer[0]));
    bytes_read = p101_read(env, err, ctx->client_socket, buffer, buffer_size - 1);

    if(p101_error_has_error(err))
    {
        next_state = ERROR;
        goto done;
    }

    buffer[bytes_read] = '\0';
    printf("Received: %s\n", buffer);
    p101_write(env, err, ctx->client_socket, "ACK", 3);

    if(p101_error_has_error(err))
    {
        next_state = ERROR;
        goto done;
    }

    next_state = CLEANUP;

done:
    p101_free(env, buffer);

    return next_state;
}

static p101_fsm_state_t cleanup_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    struct server_context *ctx;

    ctx = (struct server_context *)arg;
    p101_close(env, err, ctx->client_socket);
    ctx->client_socket = -1;

    return ACCEPT;
}

static p101_fsm_state_t error_state(const struct p101_env *env, struct p101_error *err, void *arg)
{
    const struct server_context *ctx;
    const char                  *message;

    ctx = (struct server_context *)arg;

    if(ctx->client_socket != -1)
    {
        p101_close(env, err, ctx->client_socket);
    }

    if(ctx->server_socket != -1)
    {
        p101_close(env, err, ctx->server_socket);
        p101_unlink(env, err, socket_path);
    }

    message = p101_error_get_message(err);
    fprintf(stderr, "Error: %s, shutting down.\n", message);

    return P101_FSM_EXIT;
}
