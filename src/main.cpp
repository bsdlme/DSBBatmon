/*-
 * Copyright (c) 2017 Marcel Kaiser. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <signal.h>
#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include "battindicator.h"
#include "lib/config.h"
#include "qt-helper/qt-helper.h"

static dsbcfg_t *cfg;

static void
save_config(int /* unused */)
{
	static sig_atomic_t block = 0;

	if (block == 1)
		return;
	block = 1;
	if (cfg != NULL)
		dsbcfg_write(PROGRAM, "config", cfg);
	exit(EXIT_SUCCESS);
}

int
main(int argc, char *argv[])
{

	(void)signal(SIGINT, save_config);
	(void)signal(SIGTERM, save_config);
	(void)signal(SIGQUIT, save_config);
	(void)signal(SIGHUP, save_config);

	QApplication app(argc, argv);
	QTranslator translator;

	if (translator.load(QLocale(), QLatin1String(PROGRAM),
	    QLatin1String("_"), QLatin1String(LOCALE_PATH)))
		app.installTranslator(&translator);
	cfg = dsbcfg_read(PROGRAM, "config", vardefs, CFG_NVARS);
        if (cfg == NULL && errno == ENOENT) {
                cfg = dsbcfg_new(NULL, vardefs, CFG_NVARS);
                if (cfg == NULL)
                        qh_errx(0, EXIT_FAILURE, "%s", dsbcfg_strerror());
        } else if (cfg == NULL)
                qh_errx(0, EXIT_FAILURE, "%s", dsbcfg_strerror());
	app.setQuitOnLastWindowClosed(false);
	BattIndicator bi(cfg);

	return (app.exec());
}

