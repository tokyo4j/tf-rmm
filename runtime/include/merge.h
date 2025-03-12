#ifndef MERGE_H
#define MERGE_H

#include <stdint.h>

struct rec;
struct rsi_result;
struct smc_result;

void handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res);

void smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res);

#endif
