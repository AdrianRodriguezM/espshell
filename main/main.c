/* SPDX-License-Identifier: GPL-3.0-or-later
 * espshell — application entry point.
 */
#include "core.h"
#include "project_app.h"

void app_main(void)
{
    core_init();
    project_init();
}
