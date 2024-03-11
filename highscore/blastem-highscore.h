#pragma once

#include <highscore/libhighscore.h>

G_BEGIN_DECLS

#define BLASTEM_TYPE_CORE (blastem_core_get_type())

G_DECLARE_FINAL_TYPE (BlastemCore, blastem_core, BLASTEM, CORE, HsCore)

G_MODULE_EXPORT GType hs_get_core_type (void);

G_END_DECLS
