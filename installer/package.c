#include "package.h"

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#include <stdint.h>
#include <stdio.h>

static bool started;

bool package_start(int *error) {
	if (started)
		return true;
	// The installer is built on Paf, which wants to be told how much memory
	// to take as it loads: 4 MB here, as other homebrew gives it for this.
	static uint32_t paf_args[] = {0x400000, 0xEA60, 0x40000, 0, 0};
	int paf_result = -1;
	SceSysmoduleOpt opt = {sizeof(SceSysmoduleOpt), &paf_result, {-1, -1}};
	int res = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(paf_args), paf_args, &opt);
	if (res >= 0)
		res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
	if (res >= 0)
		res = scePromoterUtilityInit();
	if (res < 0) {
		*error = res;
		return false;
	}
	started = true;
	return true;
}

// Waits for the installer to finish what it was asked, for up to a minute.
static void settle(void) {
	for (int i = 0; i < 600; i++) {
		int state = 0;
		if (scePromoterUtilityGetState(&state) < 0 || state == 0)
			return;
		sceKernelDelayThread(100 * 1000);
	}
}

int package_install(const char *dir) {
	int res = scePromoterUtilityPromotePkgWithRif(dir, 1); // 1: return when it is done
	if (res < 0)
		return res;
	settle();
	int result = 0;
	if (scePromoterUtilityGetResult(&result) >= 0 && result < 0)
		return result;
	return 0;
}

bool package_installed(const char *titleid) {
	int res = 0;
	return scePromoterUtilityCheckExist(titleid, &res) >= 0;
}

int package_remove(const char *titleid) {
	int res = scePromoterUtilityDeletePkg(titleid);
	if (res < 0)
		return res;
	settle();
	return 0;
}

void package_launch(const char *titleid) {
	char uri[40];
	snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", titleid);
	sceAppMgrLaunchAppByUri(0xFFFFF, uri);
	sceKernelDelayThread(200 * 1000);
	sceKernelExitProcess(0);
}
