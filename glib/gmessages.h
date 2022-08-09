#ifndef __G_MESSAGES_H__
#define __G_MESSAGES_H__

#define g_return_if_fail(expr) G_STMT_START{ (void)0; }G_STMT_END
#define g_return_val_if_fail(expr,val) G_STMT_START{ (void)0; }G_STMT_END
#define g_assert(expr)                  G_STMT_START { (void) 0; } G_STMT_END
#define g_assert_cmpint(n1, cmp, n2)    G_STMT_START { (void) 0; } G_STMT_END

#define TRACE(probe)


static inline void
g_error (const gchar *format,
         ...)
{
}
static inline void
g_message (const gchar *format,
           ...)
{
}
static inline void
g_critical (const gchar *format,
            ...)
{
}
static inline void
g_warning (const gchar *format,
           ...)
{
}
static inline void
g_info (const gchar *format,
        ...)
{
}
static inline void
g_debug (const gchar *format,
         ...)
{
}
#endif
