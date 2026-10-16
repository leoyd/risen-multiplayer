// KCP vendorisé de la DLL (client/third_party/kcp/, KCP 2.1.1), compilé tel quel : pas de copie.
#include "../../../client/third_party/kcp/ikcp.c"

#include <stdint.h>
#include "_cgo_export.h"

static int fg_output(const char *buf, int len, ikcpcb *kcp, void *user) {
	(void)kcp;
	return fgKcpOutput((uintptr_t)user, (char *)buf, len);
}

// Mêmes réglages que net/transport.cpp : nodelay(1,10,2,1), MTU 1199 (+ 1 octet de canal).
ikcpcb *fg_create(IUINT32 conv, uintptr_t handle) {
	ikcpcb *kcp = ikcp_create(conv, (void *)handle);
	if (kcp == NULL) return NULL;
	ikcp_setoutput(kcp, fg_output);
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	ikcp_setmtu(kcp, 1199);
	return kcp;
}
