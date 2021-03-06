//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// username.h
//
// Identification: src/include/parser/common/username.h
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


/*
 *	username.h
 *		lookup effective username
 *
 *	Copyright (c) 2003-2015, PostgreSQL Global Development Group
 *
 *	src/include/common/username.h
 */
#ifndef USERNAME_H
#define USERNAME_H

extern const char *get_user_name(char **errstr);
extern const char *get_user_name_or_exit(const char *progname);

#endif   /* USERNAME_H */
