/* dbt.h — Z80 Dynamic Binary Translator public interface
 *
 * This will be the heart of the 10 BIPS monster.
 * Modeled directly on ~/riscv/dbt/dbt.h
 */
#ifndef DBT_H
#define DBT_H

#include <stdint.h>

int dbt_jit_available(void);

#endif
