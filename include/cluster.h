#pragma once

/**
 * @file cluster.h
 * @brief Публичный API библиотеки распределённых вычислений.
 *
 * Библиотека предоставляет два интерфейса:
 * - @ref ClusterWorker  — рабочий узел, выполняет подзадачи.
 * - @ref ClusterControl — управляющий узел, распределяет работу и собирает результаты.
 */

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Коллбэк вычисления подзадачи на рабочем узле.
 *
 * Реализуется прикладным кодом. Интегрирует функцию (известную обеим
 * сторонам) на отрезке [@p range_start, @p range_end] методом средних
 * прямоугольников, используя @p num_intervals подынтервалов и @p num_cores
 * параллельных потоков.
 *
 * @param range_start  Левая граница отрезка интегрирования.
 * @param range_end    Правая граница отрезка интегрирования.
 * @param num_intervals Количество подынтервалов для численного метода.
 * @param num_cores    Количество потоков для параллельного вычисления.
 * @return Частичный результат интегрирования на данном отрезке.
 */
typedef double (*cluster_subtask_fn_t)(double   range_start,
                                       double   range_end,
                                       uint64_t num_intervals,
                                       int      num_cores);

/** @brief Тип рабочего узла. */
typedef struct ClusterWorker  ClusterWorker;

/** @brief Тип управляющего узла. */
typedef struct ClusterControl ClusterControl;

/**
 * @defgroup worker Интерфейс рабочего узла
 * @{
 */

/**
 * @brief Создать рабочий узел.
 *
 * @param max_cores   Максимальное количество ядер для вычисления.
 * @param timeout_sec Максимальное время ожидания подключения или сообщения
 *                    от управляющего узла в секундах (0 — без ограничений).
 * @param port        TCP-порт для прослушивания входящих подключений.
 * @return Указатель на созданный узел или NULL при ошибке выделения памяти.
 */
ClusterWorker *cluster_worker_create(int max_cores, int timeout_sec, uint16_t port);

/**
 * @brief Запустить рабочий узел (блокирующий вызов).
 *
 * Открывает TCP-сокет, ждёт подключения управляющего узла и входит в цикл
 * обработки задач: получает @ref MSG_TASK, вызывает @p fn, отправляет
 * @ref MSG_RESULT. Завершается при получении @ref MSG_DONE или при
 * фатальной ошибке (обрыв соединения, тайм-аут).
 *
 * При ошибке отправляет @ref MSG_ABORT управляющему узлу перед выходом.
 *
 * @param worker Указатель на рабочий узел.
 * @param fn     Коллбэк вычисления подзадачи.
 * @return true при штатном завершении, false при ошибке.
 */
bool cluster_worker_run(ClusterWorker *worker, cluster_subtask_fn_t fn);

/**
 * @brief Освободить ресурсы рабочего узла.
 *
 * Закрывает открытые файловые дескрипторы и освобождает память.
 * Безопасно вызывать если @ref cluster_worker_run не была вызвана.
 *
 * @param worker Указатель на рабочий узел (допускается NULL).
 */
void cluster_worker_destroy(ClusterWorker *worker);

/** @} */

/**
 * @defgroup control Интерфейс управляющего узла
 * @{
 */

/**
 * @brief Создать управляющий узел.
 *
 * @param num_workers_required Минимальное количество рабочих узлов,
 *                             необходимых для запуска вычисления.
 * @param timeout_sec          Максимальное время всего вычисления в секундах
 *                             (0 — без ограничений).
 * @return Указатель на созданный узел или NULL при ошибке выделения памяти.
 */
ClusterControl *cluster_control_create(int num_workers_required, int timeout_sec);

/**
 * @brief Зарегистрировать адрес рабочего узла.
 *
 * Должна вызываться до @ref cluster_control_connect.
 *
 * @param ctrl Указатель на управляющий узел.
 * @param host IPv4-адрес рабочего узла в текстовом виде (например "127.0.0.1").
 * @param port TCP-порт рабочего узла.
 * @return true при успехе, false если превышен лимит числа узлов.
 */
bool cluster_control_add_worker(ClusterControl *ctrl,
                                 const char     *host,
                                 uint16_t        port);

/**
 * @brief Подключиться ко всем зарегистрированным рабочим узлам.
 *
 * Устанавливает TCP-соединения. При неудаче повторяет попытку до 10 раз
 * с интервалом 1 секунда.
 *
 * @param ctrl Указатель на управляющий узел.
 * @return true если подключилось не менее @p num_workers_required узлов,
 *         false иначе.
 */
bool cluster_control_connect(ClusterControl *ctrl);

/**
 * @brief Вычислить интеграл методом распределённых вычислений.
 *
 * Разбивает отрезок [@p a, @p b] между рабочими узлами и итеративно
 * уточняет результат, удваивая количество подынтервалов до тех пор, пока
 * разница между двумя последовательными результатами не станет меньше
 * @p epsilon. Выводит результат в stdout.
 *
 * При обнаружении ошибки (отказ узла, тайм-аут) рассылает @ref MSG_ABORT
 * всем рабочим узлам.
 *
 * @param ctrl              Указатель на управляющий узел.
 * @param a                 Левая граница интегрирования.
 * @param b                 Правая граница интегрирования.
 * @param epsilon           Требуемая абсолютная точность.
 * @param num_intervals_init Начальное количество подынтервалов.
 * @return Значение интеграла или NAN при ошибке.
 */
double cluster_control_compute_integral(ClusterControl *ctrl,
                                         double          a,
                                         double          b,
                                         double          epsilon,
                                         uint64_t        num_intervals_init);

/**
 * @brief Отключиться от рабочих узлов и освободить ресурсы.
 *
 * Отправляет @ref MSG_DONE всем подключённым рабочим узлам, закрывает
 * сокеты и освобождает память.
 *
 * @param ctrl Указатель на управляющий узел (допускается NULL).
 */
void cluster_control_destroy(ClusterControl *ctrl);

/** @} */
