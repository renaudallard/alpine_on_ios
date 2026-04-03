/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/* Log levels */
#define LOG_LVL_ERROR	0
#define LOG_LVL_WARN	1
#define LOG_LVL_INFO	2
#define LOG_LVL_DEBUG	3
#define LOG_LVL_TRACE	4

void	log_init(int level);
void	log_set_level(int level);
void	log_msg(int level, const char *fmt, ...)
	    __attribute__((format(printf, 2, 3)));

#define LOG_ERR(...)	log_msg(LOG_LVL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)	log_msg(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_INFO(...)	log_msg(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_DBG(...)	log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...)	log_msg(LOG_LVL_TRACE, __VA_ARGS__)

#endif /* LOG_H */
