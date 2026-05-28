#pragma once

#include <stdint.h>

typedef enum {
    MSG_TASK   = 1,  // control → worker: вычисляем подзадачу
    MSG_RESULT = 2,  // worker  → control: промежуточный результат
    MSG_ABORT  = 3,  // either  → either:  ошибка
    MSG_DONE   = 4,  // control → worker:  disconnect
} MsgTag;

// отключаем автоматическое выравнивание в структурах
#pragma pack(push, 1)

typedef struct {
    uint8_t  tag;           // MSG_TASK
    uint8_t  _pad[3];       // выравниваем сами
    int32_t  idx;           // индекс этого воркера
    int32_t  total;         // всего воркеров в итерации
    uint8_t  _pad2[4];
    uint64_t n;             // суммарное количество шагов
} TaskMsg;                  // 24 bytes

typedef struct {
    uint8_t  tag;           // MSG_RESULT
    uint8_t  _pad[7];
    double   result;
    int32_t  status;        // 0 = ok, !0 = ошибка воркера
    uint8_t  _pad2[4];
} ResultMsg;                // 24 bytes

typedef struct {
    uint8_t  tag;           // MSG_ABORT or MSG_DONE
    uint8_t  _pad[7];
} ControlMsg;               // 8 bytes

#pragma pack(pop)
