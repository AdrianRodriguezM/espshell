# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
    SRCS "main.c"
         "project_app.c"
    INCLUDE_DIRS "."
    REQUIRES core
)
