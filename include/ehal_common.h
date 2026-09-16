#ifndef EHAL_COMMON_H
#define EHAL_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EHAL_OK = 0,
    EHAL_ERR_PARAM = -1,
    EHAL_ERR_STATE = -2,
    EHAL_ERR_CONFIG = -3,
    EHAL_ERR_RUNTIME = -4,
    EHAL_ERR_NOT_SUPPORTED = -5,
    EHAL_ERR_BUSY = -6,
    EHAL_ERR_TIMEOUT = -7
} ehal_result_t;

#ifdef __cplusplus
}
#endif

#endif /* EHAL_COMMON_H */
