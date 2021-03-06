// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#if __linux__ || __APPLE__
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#endif

#include "net/resolve.h"
#include "util/text/parsers.h"

#include "Common/ChunkFile.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MIPS/MIPS.h"
#include "Core/Config.h"
#include "Core/MemMapHelpers.h"
#include "Core/Util/PortManager.h"

#include "sceKernel.h"
#include "sceKernelThread.h"
#include "sceKernelMutex.h"
#include "sceUtility.h"

#include "Core/HLE/proAdhoc.h"
#include "Core/HLE/sceNetAdhoc.h"
#include "Core/HLE/sceNet.h"
#include "Core/Reporting.h"
#include "Core/Instance.h"

static bool netInited;
bool netInetInited;
bool netApctlInited;
int netApctlStatus;
u32 netDropRate = 0;
u32 netDropDuration = 0;
u32 netPoolAddr = 0;
u32 netThread1Addr = 0;
u32 netThread2Addr = 0;

static struct SceNetMallocStat netMallocStat;

static std::map<int, ApctlHandler> apctlHandlers;

static struct SceNetApctlInfoInternal netApctlInfo;

static int InitLocalhostIP() {
	// find local IP
	addrinfo* localAddr;
	addrinfo* ptr;
	char ipstr[256];
	sprintf(ipstr, "127.0.0.%u", PPSSPP_ID);
	int iResult = getaddrinfo(ipstr, 0, NULL, &localAddr);
	if (iResult != 0) {
		ERROR_LOG(SCENET, "DNS Error (%s) result: %d\n", ipstr, iResult);
		//osm.Show("DNS Error, can't resolve client bind " + ipstr, 8.0f);
		((sockaddr_in*)&LocalhostIP)->sin_family = AF_INET;
		((sockaddr_in*)&LocalhostIP)->sin_addr.s_addr = inet_addr(ipstr); //"127.0.0.1"
		((sockaddr_in*)&LocalhostIP)->sin_port = 0;
		return iResult;
	}
	for (ptr = localAddr; ptr != NULL; ptr = ptr->ai_next) {
		switch (ptr->ai_family) {
		case AF_INET:
			memcpy(&LocalhostIP, ptr->ai_addr, sizeof(sockaddr));
			break;
		}
	}
	((sockaddr_in*)&LocalhostIP)->sin_port = 0;
	freeaddrinfo(localAddr);

	// Resolve server dns
	addrinfo* resultAddr;
	in_addr serverIp;
	serverIp.s_addr = INADDR_NONE;

	iResult = getaddrinfo(g_Config.proAdhocServer.c_str(), 0, NULL, &resultAddr);
	if (iResult != 0) {
		ERROR_LOG(SCENET, "DNS Error (%s)\n", g_Config.proAdhocServer.c_str());
		return iResult;
	}
	for (ptr = resultAddr; ptr != NULL; ptr = ptr->ai_next) {
		switch (ptr->ai_family) {
		case AF_INET:
			serverIp = ((sockaddr_in*)ptr->ai_addr)->sin_addr;
			break;
		}
	}
	freeaddrinfo(resultAddr);
	isLocalServer = (((uint8_t*)&serverIp.s_addr)[0] == 0x7f);

	return 0;
}

static void __ResetInitNetLib() {
	netInited = false;
	netApctlInited = false;
	netInetInited = false;

	memset(&netMallocStat, 0, sizeof(netMallocStat));
	memset(&parameter, 0, sizeof(parameter));
}

void __NetInit() {
	// Windows: Assuming WSAStartup already called beforehand
	portOffset = g_Config.iPortOffset;
	isOriPort = g_Config.bEnableUPnP && g_Config.bUPnPUseOriginalPort;
	minSocketTimeoutUS = g_Config.iMinTimeout * 1000UL;

	InitLocalhostIP();

	SceNetEtherAddr mac;
	getLocalMac(&mac);
	INFO_LOG(SCENET, "LocalHost IP will be %s [%s]", inet_ntoa(((sockaddr_in*)&LocalhostIP)->sin_addr), mac2str(&mac).c_str());
	
	// TODO: May be we should initialize & cleanup somewhere else than here for PortManager to be used as general purpose for whatever port forwarding PPSSPP needed
	__UPnPInit();

	__ResetInitNetLib();
}

void __NetShutdown() {
	// Network Cleanup
	Net_Term();

	__ResetInitNetLib();

	// Since PortManager supposed to be general purpose for whatever port forwarding PPSSPP needed, may be we shouldn't clear & restore ports in here? it will be cleared and restored by PortManager's destructor when exiting PPSSPP anyway
	__UPnPShutdown();
}

static void __UpdateApctlHandlers(int oldState, int newState, int flag, int error) {
	u32 args[5] = { 0, 0, 0, 0, 0 };
		args[0] = oldState;
		args[1] = newState;
		args[2] = flag;
		args[3] = error;

	for(std::map<int, ApctlHandler>::iterator it = apctlHandlers.begin(); it != apctlHandlers.end(); ++it) {
		args[4] = it->second.argument;
		// TODO: Need to make sure netApctlState is updated before calling the callback's mipscall so the game handler's subroutine can Get and make use the new state/info
		hleEnqueueCall(it->second.entryPoint, 5, args);
	}
}

// This feels like a dubious proposition, mostly...
void __NetDoState(PointerWrap &p) {
	auto s = p.Section("sceNet", 1, 3);
	if (!s)
		return;

	auto cur_netInited = netInited;
	auto cur_netInetInited = netInetInited;
	auto cur_netApctlInited = netApctlInited;

	p.Do(netInited);
	p.Do(netInetInited);
	p.Do(netApctlInited);
	p.Do(apctlHandlers);
	p.Do(netMallocStat);
	if (s < 2) {
		netDropRate = 0;
		netDropDuration = 0;
	} else {
		p.Do(netDropRate);
		p.Do(netDropDuration);
	}
	if (s < 3) {
		netPoolAddr = 0;
		netThread1Addr = 0;
		netThread2Addr = 0;
	} else {
		p.Do(netPoolAddr);
		p.Do(netThread1Addr);
		p.Do(netThread2Addr);
	}
	// Let's not change "Inited" value when Loading SaveState in the middle of multiplayer to prevent memory & port leaks
	if (p.mode == p.MODE_READ) {
		netApctlInited = cur_netApctlInited;
		netInetInited = cur_netInetInited;
		netInited = cur_netInited;
	}
}

template <typename I> std::string num2hex(I w, size_t hex_len) {
	static const char* digits = "0123456789ABCDEF";
	std::string rc(hex_len, '0');
	for (size_t i = 0, j = (hex_len - 1) * 4; i < hex_len; ++i, j -= 4)
		rc[i] = digits[(w >> j) & 0x0f];
	return rc;
}

std::string error2str(u32 errorCode) {
	std::string str = "";
	if (((errorCode >> 31) & 1) != 0)
		str += "ERROR ";
	if (((errorCode >> 30) & 1) != 0)
		str += "CRITICAL ";
	switch ((errorCode >> 16) & 0xfff) {
	case 0x41:
		str += "NET ";
		break;
	default:
		str += "UNK"+num2hex(u16((errorCode >> 16) & 0xfff), 3)+" ";
	}
	switch ((errorCode >> 8) & 0xff) {
	case 0x00:
		str += "COMMON ";
		break;
	case 0x01:
		str += "CORE ";
		break;
	case 0x02:
		str += "INET ";
		break;
	case 0x03:
		str += "POECLIENT ";
		break;
	case 0x04:
		str += "RESOLVER ";
		break;
	case 0x05:
		str += "DHCP ";
		break;
	case 0x06:
		str += "ADHOC_AUTH ";
		break;
	case 0x07:
		str += "ADHOC ";
		break;
	case 0x08:
		str += "ADHOC_MATCHING ";
		break;
	case 0x09:
		str += "NETCNF ";
		break;
	case 0x0a:
		str += "APCTL ";
		break;
	case 0x0b:
		str += "ADHOCCTL ";
		break;
	case 0x0c:
		str += "UNKNOWN1 ";
		break;
	case 0x0d:
		str += "WLAN ";
		break;
	case 0x0e:
		str += "EAPOL ";
		break;
	case 0x0f:
		str += "8021x ";
		break;
	case 0x10:
		str += "WPA ";
		break;
	case 0x11:
		str += "UNKNOWN2 ";
		break;
	case 0x12:
		str += "TRANSFER ";
		break;
	case 0x13:
		str += "ADHOC_DISCOVER ";
		break;
	case 0x14:
		str += "ADHOC_DIALOG ";
		break;
	case 0x15:
		str += "WISPR ";
		break;
	default:
		str += "UNKNOWN"+num2hex(u8((errorCode >> 8) & 0xff))+" ";
	}
	str += num2hex(u8(errorCode & 0xff));
	return str;
}

static inline u32 AllocUser(u32 size, bool fromTop, const char *name) {
	u32 addr = userMemory.Alloc(size, fromTop, name);
	if (addr == -1)
		return 0;
	return addr;
}

static inline void FreeUser(u32 &addr) {
	if (addr != 0)
		userMemory.Free(addr);
	addr = 0;
}

u32 Net_Term() {
	// May also need to Terminate netAdhocctl and netAdhoc to free some resources & threads, since the game (ie. GTA:VCS, Wipeout Pulse, etc) might not called them before calling sceNetTerm and causing them to behave strangely on the next sceNetInit & sceNetAdhocInit
	NetAdhocctl_Term();
	NetAdhoc_Term();

	// TODO: Not implemented yet
	//sceNetApctl_Term();
	//NetInet_Term();

	// Library is initialized
	if (netInited) {
		// Delete PDP Sockets
		deleteAllPDP();

		// Delete PTP Sockets
		deleteAllPTP();

		// Delete GameMode Buffer
		//deleteAllGMB();

		// Terminate Internet Library
		//sceNetInetTerm();

		// Unload Internet Modules (Just keep it in memory... unloading crashes?!)
		// if (_manage_modules != 0) sceUtilityUnloadModule(PSP_MODULE_NET_INET);
		// Library shutdown
	}

	FreeUser(netPoolAddr);
	FreeUser(netThread1Addr);
	FreeUser(netThread2Addr);
	netInited = false;

	return 0;
}

static u32 sceNetTerm() {
	WARN_LOG(SCENET, "sceNetTerm()");
	int retval = Net_Term();

	// Give time to make sure everything are cleaned up
	hleDelayResult(retval, "give time to init/cleanup", adhocEventDelayMS * 1000);
	return retval;
}

/*
Parameters:
	poolsize	- Memory pool size (appears to be for the whole of the networking library).
	calloutprio	- Priority of the SceNetCallout thread.
	calloutstack	- Stack size of the SceNetCallout thread (defaults to 4096 on non 1.5 firmware regardless of what value is passed).
	netintrprio	- Priority of the SceNetNetintr thread.
	netintrstack	- Stack size of the SceNetNetintr thread (defaults to 4096 on non 1.5 firmware regardless of what value is passed).
*/
static int sceNetInit(u32 poolSize, u32 calloutPri, u32 calloutStack, u32 netinitPri, u32 netinitStack)  {
	// TODO: Create Network Threads using given priority & stack
	// TODO: The correct behavior is actually to allocate more and leak the other threads/pool.
	// But we reset here for historic reasons (GTA:VCS potentially triggers this.)
	if (netInited)
		Net_Term(); // This cleanup attempt might not worked when SaveState were loaded in the middle of multiplayer game and re-entering multiplayer, thus causing memory leaks & wasting binded ports. May be we shouldn't save/load "Inited" vars on SaveState?

	if (poolSize == 0) {
		return hleLogError(SCENET, SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE, "invalid pool size");
	} else if (calloutPri < 0x08 || calloutPri > 0x77) {
		return hleLogError(SCENET, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY, "invalid callout thread priority");
	} else if (netinitPri < 0x08 || netinitPri > 0x77) {
		return hleLogError(SCENET, SCE_KERNEL_ERROR_ILLEGAL_PRIORITY, "invalid init thread priority");
	}

	// TODO: Should also start the threads, probably?  For now, let's just allocate.
	// TODO: Respect the stack size if firmware set to 1.50?
	u32 stackSize = 4096;
	netThread1Addr = AllocUser(stackSize, true, "netstack1");
	if (netThread1Addr == 0) {
		return hleLogError(SCENET, SCE_KERNEL_ERROR_NO_MEMORY, "unable to allocate thread");
	}
	netThread2Addr = AllocUser(stackSize, true, "netstack2");
	if (netThread2Addr == 0) {
		FreeUser(netThread1Addr);
		return hleLogError(SCENET, SCE_KERNEL_ERROR_NO_MEMORY, "unable to allocate thread");
	}

	netPoolAddr = AllocUser(poolSize, false, "netpool");
	if (netPoolAddr == 0) {
		FreeUser(netThread1Addr);
		FreeUser(netThread2Addr);
		return hleLogError(SCENET, SCE_KERNEL_ERROR_NO_MEMORY, "unable to allocate pool");
	}

	WARN_LOG(SCENET, "sceNetInit(poolsize=%d, calloutpri=%i, calloutstack=%d, netintrpri=%i, netintrstack=%d) at %08x", poolSize, calloutPri, calloutStack, netinitPri, netinitStack, currentMIPS->pc);
	netInited = true;
	netMallocStat.pool = poolSize; // This should be the poolSize isn't?
	netMallocStat.maximum = poolSize/2; // According to JPCSP's sceNetGetMallocStat this is Currently Used size = (poolSize - free), faked to half the pool
	netMallocStat.free = poolSize - netMallocStat.maximum;

	// Clear Socket Translator Memory
	memset(&pdp, 0, sizeof(pdp));
	memset(&ptp, 0, sizeof(ptp));
	
	return hleLogSuccessI(SCENET, 0);
}

// Free(delete) thread info / data. 
// Normal usage: sceKernelDeleteThread followed by sceNetFreeThreadInfo with the same threadID as argument
static int sceNetFreeThreadinfo(SceUID thid) {
	ERROR_LOG(SCENET, "UNIMPL sceNetFreeThreadinfo(%i)", thid);

	return 0;
}

// Abort a thread.
static int sceNetThreadAbort(SceUID thid) {
	ERROR_LOG(SCENET, "UNIMPL sceNetThreadAbort(%i)", thid);

	return 0;
}

static u32 sceWlanGetEtherAddr(u32 addrAddr) {
	if (!Memory::IsValidRange(addrAddr, 6)) {
		// More correctly, it should crash.
		return hleLogError(SCENET, SCE_KERNEL_ERROR_ILLEGAL_ADDR, "illegal address");
	}

	u8 *addr = Memory::GetPointer(addrAddr);
	if (PPSSPP_ID > 1) {
		Memory::Memset(addrAddr, PPSSPP_ID, 6);
	}
	else
	// Read MAC Address from config
	if (!ParseMacAddress(g_Config.sMACAddress.c_str(), addr)) {
		ERROR_LOG(SCENET, "Error parsing mac address %s", g_Config.sMACAddress.c_str());
		Memory::Memset(addrAddr, 0, 6);
	} else {
		CBreakPoints::ExecMemCheck(addrAddr, true, 6, currentMIPS->pc);
	}

	return hleLogSuccessI(SCENET, hleDelayResult(0, "get ether mac", 200));
}

static u32 sceNetGetLocalEtherAddr(u32 addrAddr) {
	return sceWlanGetEtherAddr(addrAddr);
}

static u32 sceWlanDevIsPowerOn() {
	return hleLogSuccessVerboseI(SCENET, g_Config.bEnableWlan ? 1 : 0);
}

static u32 sceWlanGetSwitchState() {
	return hleLogSuccessVerboseI(SCENET, g_Config.bEnableWlan ? 1 : 0);
}

// Probably a void function, but often returns a useful value.
static void sceNetEtherNtostr(u32 macPtr, u32 bufferPtr) {
	DEBUG_LOG(SCENET, "sceNetEtherNtostr(%08x, %08x)", macPtr, bufferPtr);

	if (Memory::IsValidAddress(bufferPtr) && Memory::IsValidAddress(macPtr)) {
		char *buffer = (char *)Memory::GetPointer(bufferPtr);
		const u8 *mac = Memory::GetPointer(macPtr);

		// MAC address is always 6 bytes / 48 bits.
		sprintf(buffer, "%02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

		VERBOSE_LOG(SCENET, "sceNetEtherNtostr - [%s]", buffer);
	}
}

static int hex_to_digit(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

// Probably a void function, but sometimes returns a useful-ish value.
static void sceNetEtherStrton(u32 bufferPtr, u32 macPtr) {
	DEBUG_LOG(SCENET, "sceNetEtherStrton(%08x, %08x)", bufferPtr, macPtr);

	if (Memory::IsValidAddress(bufferPtr) && Memory::IsValidAddress(macPtr)) {
		const char *buffer = (char *)Memory::GetPointer(bufferPtr);
		u8 *mac = Memory::GetPointer(macPtr);

		// MAC address is always 6 pairs of hex digits.
		// TODO: Funny stuff happens if it's too short.
		u8 value = 0;
		for (int i = 0; i < 6 && *buffer != 0; ++i) {
			value = 0;

			int c = hex_to_digit(*buffer++);
			if (c != -1) {
				value |= c << 4;
			}
			c = hex_to_digit(*buffer++);
			if (c != -1) {
				value |= c;
			}

			*mac++ = value;

			// Skip a single character in between.
			// TODO: Strange behavior on the PSP, let's just null check.
			if (*buffer++ == 0) {
				break;
			}
		}

		VERBOSE_LOG(SCENET, "sceNetEtherStrton - [%s]", mac2str((SceNetEtherAddr*)Memory::GetPointer(macPtr)).c_str());
		// Seems to maybe kinda return the last value.  Probably returns void.
		//return value;
	}
}


// Write static data since we don't actually manage any memory for sceNet* yet.
static int sceNetGetMallocStat(u32 statPtr) {
	WARN_LOG(SCENET, "UNTESTED sceNetGetMallocStat(%x)", statPtr);
	if(Memory::IsValidAddress(statPtr))
		Memory::WriteStruct(statPtr, &netMallocStat);
	else
		ERROR_LOG(SCENET, "UNTESTED sceNetGetMallocStat(%x): tried to request invalid address!", statPtr);

	return 0;
}

static int sceNetInetInit() {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetInit()");
	if (netInetInited) return ERROR_NET_INET_ALREADY_INITIALIZED;
	netInetInited = true;

	return 0;
}

int sceNetInetTerm() {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetTerm()");
	netInetInited = false;

	return 0;
}

static int sceNetApctlInit(int stackSize, int initPriority) {
	ERROR_LOG(SCENET, "UNIMPL sceNetApctlInit()");
	if (netApctlInited)
		return ERROR_NET_APCTL_ALREADY_INITIALIZED;

	memset(&netApctlInfo, 0, sizeof(netApctlInfo));
	// Set default values
	truncate_cpy(netApctlInfo.name, sizeof(netApctlInfo.name), "Wifi");
	truncate_cpy(netApctlInfo.ssid, sizeof(netApctlInfo.ssid), "Wifi");
	memcpy(netApctlInfo.bssid, "\1\1\2\2\3\3", sizeof(netApctlInfo.bssid));
	netApctlInfo.ssidLength = 4;
	netApctlInfo.strength = 99;
	netApctlInfo.channel = g_Config.iWlanAdhocChannel;
	if (netApctlInfo.channel == PSP_SYSTEMPARAM_ADHOC_CHANNEL_AUTOMATIC) netApctlInfo.channel = defaultWlanChannel;
	// Get Local IP Address
	sockaddr_in sockAddr;
	getLocalIp(&sockAddr);
	char ipstr[INET_ADDRSTRLEN] = "127.0.0.1"; // default
	inet_ntop(AF_INET, &sockAddr.sin_addr, ipstr, sizeof(ipstr));
	truncate_cpy(netApctlInfo.ip, sizeof(netApctlInfo.ip), ipstr);
	// Change the last number to 1 to indicate a common dns server/internet gateway
	((u8*)&sockAddr.sin_addr.s_addr)[3] = 1;
	inet_ntop(AF_INET, &sockAddr.sin_addr, ipstr, sizeof(ipstr));
	truncate_cpy(netApctlInfo.gateway, sizeof(netApctlInfo.gateway), ipstr);
	truncate_cpy(netApctlInfo.primaryDns, sizeof(netApctlInfo.primaryDns), ipstr);
	truncate_cpy(netApctlInfo.secondaryDns, sizeof(netApctlInfo.secondaryDns), "8.8.8.8");
	truncate_cpy(netApctlInfo.subNetMask, sizeof(netApctlInfo.subNetMask), "255.255.255.0");

	// TODO: Create APctl fake-Thread

	netApctlInited = true;
	netApctlStatus = PSP_NET_APCTL_STATE_DISCONNECTED;

	return 0;
}

int sceNetApctlTerm() {
	ERROR_LOG(SCENET, "UNIMPL sceNeApctlTerm()");
	netApctlInited = false;
	netApctlStatus = PSP_NET_APCTL_STATE_DISCONNECTED;
	
	return 0;
}

static int sceNetApctlGetInfo(int code, u32 pInfoAddr) {
	WARN_LOG(SCENET, "UNTESTED %s(%i, %08x)", __FUNCTION__, code, pInfoAddr);

	if (!netApctlInited)
		return hleLogError(SCENET, ERROR_NET_APCTL_NOT_IN_BSS, "apctl not in bss"); // Only have valid info after joining an AP and got an IP, right?

	if (!Memory::IsValidAddress(pInfoAddr))
		return hleLogError(SCENET, -1, "apctl invalid arg");

	u8* info = Memory::GetPointer(pInfoAddr); // FIXME: Points to a union instead of a struct thus each field have the same address

	switch (code) {
	case PSP_NET_APCTL_INFO_PROFILE_NAME:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.name);
		DEBUG_LOG(SCENET, "ApctlInfo - ProfileName: %s", netApctlInfo.name);
		break;
	case PSP_NET_APCTL_INFO_BSSID:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.bssid);
		DEBUG_LOG(SCENET, "ApctlInfo - BSSID: %s", mac2str((SceNetEtherAddr*)&netApctlInfo.bssid).c_str());
		break;
	case PSP_NET_APCTL_INFO_SSID:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.ssid);
		DEBUG_LOG(SCENET, "ApctlInfo - SSID: %s", netApctlInfo.ssid);
		break;
	case PSP_NET_APCTL_INFO_SSID_LENGTH:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.ssidLength);
		break;
	case PSP_NET_APCTL_INFO_SECURITY_TYPE:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.securityType);
		break;
	case PSP_NET_APCTL_INFO_STRENGTH:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.strength);
		break;
	case PSP_NET_APCTL_INFO_CHANNEL:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.channel);
		break;
	case PSP_NET_APCTL_INFO_POWER_SAVE:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.powerSave);
		break;
	case PSP_NET_APCTL_INFO_IP:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.ip);
		DEBUG_LOG(SCENET, "ApctlInfo - IP: %s", netApctlInfo.ip);
		break;
	case PSP_NET_APCTL_INFO_SUBNETMASK:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.subNetMask);
		DEBUG_LOG(SCENET, "ApctlInfo - SubNet Mask: %s", netApctlInfo.subNetMask);
		break;
	case PSP_NET_APCTL_INFO_GATEWAY:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.gateway);
		DEBUG_LOG(SCENET, "ApctlInfo - Gateway IP: %s", netApctlInfo.gateway);
		break;
	case PSP_NET_APCTL_INFO_PRIMDNS:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.primaryDns);
		DEBUG_LOG(SCENET, "ApctlInfo - Primary DNS: %s", netApctlInfo.primaryDns);
		break;
	case PSP_NET_APCTL_INFO_SECDNS:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.secondaryDns);
		DEBUG_LOG(SCENET, "ApctlInfo - Secondary DNS: %s", netApctlInfo.secondaryDns);
		break;
	case PSP_NET_APCTL_INFO_USE_PROXY:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.useProxy);
		break;
	case PSP_NET_APCTL_INFO_PROXY_URL:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.proxyUrl);
		DEBUG_LOG(SCENET, "ApctlInfo - Proxy URL: %s", netApctlInfo.proxyUrl);
		break;
	case PSP_NET_APCTL_INFO_PROXY_PORT:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.proxyPort);
		break;
	case PSP_NET_APCTL_INFO_8021_EAP_TYPE:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.eapType);
		break;
	case PSP_NET_APCTL_INFO_START_BROWSER:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.startBrowser);
		break;
	case PSP_NET_APCTL_INFO_WIFISP:
		Memory::WriteStruct(pInfoAddr, &netApctlInfo.wifisp);
		break;
	default:
		return hleLogError(SCENET, ERROR_NET_APCTL_INVALID_CODE, "apctl invalid code");
	}

	return hleLogSuccessI(SCENET, 0);
}

// TODO: How many handlers can the PSP actually have for Apctl?
// TODO: Should we allow the same handler to be added more than once?
static u32 sceNetApctlAddHandler(u32 handlerPtr, u32 handlerArg) {
	bool foundHandler = false;
	u32 retval = 0;
	struct ApctlHandler handler;
	memset(&handler, 0, sizeof(handler));

	while (apctlHandlers.find(retval) != apctlHandlers.end())
		++retval;

	handler.entryPoint = handlerPtr;
	handler.argument = handlerArg;

	for(std::map<int, ApctlHandler>::iterator it = apctlHandlers.begin(); it != apctlHandlers.end(); it++) {
		if(it->second.entryPoint == handlerPtr) {
			foundHandler = true;
			break;
		}
	}

	if(!foundHandler && Memory::IsValidAddress(handlerPtr)) {
		if(apctlHandlers.size() >= MAX_APCTL_HANDLERS) {
			ERROR_LOG(SCENET, "UNTESTED sceNetApctlAddHandler(%x, %x): Too many handlers", handlerPtr, handlerArg);
			retval = ERROR_NET_ADHOCCTL_TOO_MANY_HANDLERS; // TODO: What's the proper error code for Apctl's TOO_MANY_HANDLERS?
			return retval;
		}
		apctlHandlers[retval] = handler;
		WARN_LOG(SCENET, "UNTESTED sceNetApctlAddHandler(%x, %x): added handler %d", handlerPtr, handlerArg, retval);
	}
	else {
		ERROR_LOG(SCENET, "UNTESTED sceNetApctlAddHandler(%x, %x): Same handler already exists", handlerPtr, handlerArg);
	}

	// The id to return is the number of handlers currently registered
	return retval;
}

static int sceNetApctlDelHandler(u32 handlerID) {
	if(apctlHandlers.find(handlerID) != apctlHandlers.end()) {
		apctlHandlers.erase(handlerID);
		WARN_LOG(SCENET, "UNTESTED sceNetapctlDelHandler(%d): deleted handler %d", handlerID, handlerID);
	}
	else {
		ERROR_LOG(SCENET, "UNTESTED sceNetapctlDelHandler(%d): asked to delete invalid handler %d", handlerID, handlerID);
	}
	return 0;
}

static int sceNetInetInetAton(const char *hostname, u32 addrPtr) {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetInetAton(%s, %08x)", hostname, addrPtr);
	return -1;
}

int sceNetInetPoll(void *fds, u32 nfds, int timeout) { // timeout in miliseconds
	DEBUG_LOG(SCENET, "UNTESTED sceNetInetPoll(%p, %d, %i) at %08x", fds, nfds, timeout, currentMIPS->pc);
	int retval = -1;
	SceNetInetPollfd *fdarray = (SceNetInetPollfd *)fds; // SceNetInetPollfd/pollfd, sceNetInetPoll() have similarity to BSD poll() but pollfd have different size on 64bit
//#ifdef _WIN32
	//WSAPoll only available for Vista or newer, so we'll use an alternative way for XP since Windows doesn't have poll function like *NIX
	if (nfds > FD_SETSIZE) return -1;
	fd_set readfds, writefds, exceptfds;
	FD_ZERO(&readfds); FD_ZERO(&writefds); FD_ZERO(&exceptfds);
	for (int i = 0; i < (s32)nfds; i++) {
		if (fdarray[i].events & (INET_POLLRDNORM)) FD_SET(fdarray[i].fd, &readfds); // (POLLRDNORM | POLLIN)
		if (fdarray[i].events & (INET_POLLWRNORM)) FD_SET(fdarray[i].fd, &writefds); // (POLLWRNORM | POLLOUT)
		//if (fdarray[i].events & (ADHOC_EV_ALERT)) // (POLLRDBAND | POLLPRI) // POLLERR 
		FD_SET(fdarray[i].fd, &exceptfds); 
		fdarray[i].revents = 0;
	}
	timeval tmout;
	tmout.tv_sec = timeout / 1000; // seconds
	tmout.tv_usec = (timeout % 1000) * 1000; // microseconds
	retval = select(nfds, &readfds, &writefds, &exceptfds, &tmout);
	if (retval < 0) return -1;
	retval = 0;
	for (int i = 0; i < (s32)nfds; i++) {
		if (FD_ISSET(fdarray[i].fd, &readfds)) fdarray[i].revents |= INET_POLLRDNORM; //POLLIN
		if (FD_ISSET(fdarray[i].fd, &writefds)) fdarray[i].revents |= INET_POLLWRNORM; //POLLOUT
		fdarray[i].revents &= fdarray[i].events;
		if (FD_ISSET(fdarray[i].fd, &exceptfds)) fdarray[i].revents |= ADHOC_EV_ALERT; // POLLPRI; // POLLERR; // can be raised on revents regardless of events bitmask?
		if (fdarray[i].revents) retval++;
	}
//#else
	/*
	// Doesn't work properly yet
	pollfd *fdtmp = (pollfd *)malloc(sizeof(pollfd) * nfds);
	// Note: sizeof(pollfd) = 16bytes in 64bit and 8bytes in 32bit, while sizeof(SceNetInetPollfd) is always 8bytes
	for (int i = 0; i < (s32)nfds; i++) {
		fdtmp[i].fd = fdarray[i].fd;
		fdtmp[i].events = 0;
		if (fdarray[i].events & INET_POLLRDNORM) fdtmp[i].events |= (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI);
		if (fdarray[i].events & INET_POLLWRNORM) fdtmp[i].events |= (POLLOUT | POLLWRNORM | POLLWRBAND);
		fdtmp[i].revents = 0;
		fdarray[i].revents = 0;
	}
	retval = poll(fdtmp, (nfds_t)nfds, timeout); //retval = WSAPoll(fdarray, nfds, timeout);
	for (int i = 0; i < (s32)nfds; i++) {
		if (fdtmp[i].revents & (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI)) fdarray[i].revents |= INET_POLLRDNORM;
		if (fdtmp[i].revents & (POLLOUT | POLLWRNORM | POLLWRBAND)) fdarray[i].revents |= INET_POLLWRNORM;
		fdarray[i].revents &= fdarray[i].events;
		if (fdtmp[i].revents & POLLERR) fdarray[i].revents |= POLLERR; //INET_POLLERR // can be raised on revents regardless of events bitmask?
	}
	free(fdtmp);
	*/
//#endif
	return retval;
}

static int sceNetInetRecv(int socket, u32 bufPtr, u32 bufLen, u32 flags) {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetRecv(%i, %08x, %i, %08x)", socket, bufPtr, bufLen, flags);
	return -1;
}

static int sceNetInetSend(int socket, u32 bufPtr, u32 bufLen, u32 flags) {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetSend(%i, %08x, %i, %08x)", socket, bufPtr, bufLen, flags);
	return -1;
}

static int sceNetInetGetErrno() {
	ERROR_LOG(SCENET, "UNTESTED sceNetInetGetErrno()");
	int error = errno;
	switch (error) {
	case ETIMEDOUT:		
		return INET_ETIMEDOUT;
	case EISCONN:		
		return INET_EISCONN;
	case EINPROGRESS:	
		return INET_EINPROGRESS;
	//case EAGAIN:
	//	return INET_EAGAIN;
	}
	return error; //-1;
}

static int sceNetInetSocket(int domain, int type, int protocol) {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetSocket(%i, %i, %i)", domain, type, protocol);
	return -1;
}

static int sceNetInetSetsockopt(int socket, int level, int optname, u32 optvalPtr, int optlen) {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetSetsockopt(%i, %i, %i, %08x, %i)", socket, level, optname, optvalPtr, optlen);
	return -1;
}

static int sceNetInetConnect(int socket, u32 sockAddrInternetPtr, int addressLength) {
	ERROR_LOG(SCENET, "UNIMPL sceNetInetConnect(%i, %08x, %i)", socket, sockAddrInternetPtr, addressLength);
	return -1;
}

static int sceNetApctlConnect(int connIndex) {
	ERROR_LOG(SCENET, "UNIMPL %s(%i)", __FUNCTION__, connIndex);

	netApctlStatus = PSP_NET_APCTL_STATE_JOINING;
	__UpdateApctlHandlers(0, 0, PSP_NET_APCTL_EVENT_CONNECT_REQUEST, 0);
	return 0;
}

static int sceNetApctlDisconnect() {
	ERROR_LOG(SCENET, "UNIMPL %s()", __FUNCTION__);
	// Like its 'sister' function sceNetAdhocctlDisconnect, we need to alert Apctl handlers that a disconnect took place
	// or else games like Phantasy Star Portable 2 will hang at certain points (e.g. returning to the main menu after trying to connect to PSN).
	netApctlStatus = PSP_NET_APCTL_STATE_DISCONNECTED;
	__UpdateApctlHandlers(0, 0, PSP_NET_APCTL_EVENT_DISCONNECT_REQUEST, 0);
	return 0;
}

static int sceNetApctlGetState(u32 pStateAddr) {
	WARN_LOG(SCENET, "UNTESTED %s(%08x)", __FUNCTION__, pStateAddr);
	
	//if (!netApctlInited) return hleLogError(SCENET, ERROR_NET_APCTL_NOT_IN_BSS, "apctl not in bss");

	// Valid Arguments
	if (Memory::IsValidAddress(pStateAddr)) {
		// Return Thread Status
		Memory::Write_U32(netApctlStatus, pStateAddr);
		// Return Success
		return hleLogSuccessI(SCENET, 0);
	}

	return hleLogError(SCENET, -1, "apctl invalid arg");
}

static int sceNetApctlScanUser() {
	ERROR_LOG(SCENET, "UNIMPL %s()", __FUNCTION__);

	// Scan probably only works when not in connected state, right?
	if (netApctlStatus != PSP_NET_APCTL_STATE_DISCONNECTED)
		return hleLogError(SCENET, ERROR_NET_APCTL_NOT_DISCONNECTED, "apctl not disconnected");

	netApctlStatus = PSP_NET_APCTL_STATE_SCANNING;
	__UpdateApctlHandlers(0, 0, PSP_NET_APCTL_EVENT_SCAN_REQUEST, 0);
	return 0;
}

static int sceNetApctlGetBSSDescIDListUser(u32 sizeAddr, u32 bufAddr) {
	WARN_LOG(SCENET, "UNTESTED %s(%08x, %08x)", __FUNCTION__, sizeAddr, bufAddr);

	const int userInfoSize = 8;
	int entries = 1;
	if (!Memory::IsValidAddress(sizeAddr))
		hleLogError(SCENET, -1, "apctl invalid arg");

	int size = Memory::Read_U32(sizeAddr);
	// Return size required
	Memory::Write_U32(entries * userInfoSize, sizeAddr);

	if (bufAddr != 0 && Memory::IsValidAddress(sizeAddr)) {
		int offset = 0;
		for (int i = 0; i < entries; i++) {
			// Check if enough space available to write the next structure
			if (offset + userInfoSize > size) {
				break;
			}

			DEBUG_LOG(SCENET, "%s returning %d at %08x", __FUNCTION__, i, bufAddr + offset);

			// Pointer to next Network structure in list
			Memory::Write_U32((i+1)*userInfoSize + bufAddr, bufAddr + offset);
			offset += 4;

			// Entry ID
			Memory::Write_U32(i, bufAddr + offset);
			offset += 4;
		}
		// Fix the last Pointer
		if (offset > 0)
			Memory::Write_U32(0, bufAddr + offset - userInfoSize);
	}

	return hleLogWarning(SCENET, 0, "untested");
}

static int sceNetApctlGetBSSDescEntryUser(int entryId, int infoId, u32 resultAddr) {
	WARN_LOG(SCENET, "UNTESTED %s(%i, %i, %08x)", __FUNCTION__, entryId, infoId, resultAddr);

	if (!Memory::IsValidAddress(resultAddr))
		hleLogError(SCENET, -1, "apctl invalid arg");

	switch (infoId) {
	case PSP_NET_APCTL_DESC_IBSS: // IBSS, 6 bytes
		Memory::WriteStruct(resultAddr, &netApctlInfo.bssid);
		break;
	case PSP_NET_APCTL_DESC_SSID_NAME:
		// Return 32 bytes
		Memory::WriteStruct(resultAddr, &netApctlInfo.ssid);
		break;
	case PSP_NET_APCTL_DESC_SSID_NAME_LENGTH:
		// Return one 32-bit value
		Memory::WriteStruct(resultAddr, &netApctlInfo.ssidLength);
		break;
	case PSP_NET_APCTL_DESC_SIGNAL_STRENGTH:
		// Return 1 byte
		Memory::WriteStruct(resultAddr, &netApctlInfo.strength);
		break;
	case PSP_NET_APCTL_DESC_SECURITY:
		// Return one 32-bit value
		Memory::WriteStruct(resultAddr, &netApctlInfo.securityType);
		break;
	default:
		return hleLogError(SCENET, ERROR_NET_APCTL_INVALID_CODE, "unknown info id");
	}

	return hleLogWarning(SCENET, 0, "untested");
}

static int sceNetApctlScanSSID2() {
	ERROR_LOG(SCENET, "UNIMPL %s()", __FUNCTION__);

	// Scan probably only works when not in connected state, right?
	if (netApctlStatus != PSP_NET_APCTL_STATE_DISCONNECTED)
		return hleLogError(SCENET, ERROR_NET_APCTL_NOT_DISCONNECTED, "apctl not disconnected");

	netApctlStatus = PSP_NET_APCTL_STATE_SCANNING;
	__UpdateApctlHandlers(0, 0, PSP_NET_APCTL_EVENT_SCAN_REQUEST, 0);
	return 0;
}

static int sceNetApctlGetBSSDescIDList2(u32 Arg1, u32 Arg2, u32 Arg3, u32 Arg4) {
	return hleLogError(SCENET, 0, "unimplemented");
}

static int sceNetApctlGetBSSDescEntry2(u32 Arg1, u32 Arg2, u32 Arg3, u32 Arg4) {
	return hleLogError(SCENET, 0, "unimplemented");
}

static int sceNetResolverInit()
{
	ERROR_LOG(SCENET, "UNIMPL %s()", __FUNCTION__);
	return 0;
}

static int sceNetApctlAddInternalHandler(u32 handlerPtr, u32 handlerArg) {
	ERROR_LOG(SCENET, "UNIMPL %s(%08x, %08x)", __FUNCTION__, handlerPtr, handlerArg);
	// This seems to be a 2nd kind of handler
	return sceNetApctlAddHandler(handlerPtr, handlerArg);
}

static int sceNetApctlDelInternalHandler(u32 handlerID) {
	ERROR_LOG(SCENET, "UNIMPL %s(%i)", __FUNCTION__, handlerID);
	// This seems to be a 2nd kind of handler
	return sceNetApctlDelHandler(handlerID);
}

static int sceNetApctl_A7BB73DF(u32 handlerPtr, u32 handlerArg) {
	ERROR_LOG(SCENET, "UNIMPL %s(%08x, %08x)", __FUNCTION__, handlerPtr, handlerArg);
	// This seems to be a 3rd kind of handler
	return sceNetApctlAddHandler(handlerPtr, handlerArg);
}

static int sceNetApctl_6F5D2981(u32 handlerID) {
	ERROR_LOG(SCENET, "UNIMPL %s(%i)", __FUNCTION__, handlerID);
	// This seems to be a 3rd kind of handler
	return sceNetApctlDelHandler(handlerID);
}

static int sceNetApctl_lib2_69745F0A(int handlerId) {
	return hleLogError(SCENET, 0, "unimplemented");
}

static int sceNetApctl_lib2_4C19731F(int code, u32 pInfoAddr) {
	ERROR_LOG(SCENET, "UNIMPL %s(%i, %08x)", __FUNCTION__, code, pInfoAddr);
	return sceNetApctlGetInfo(code, pInfoAddr);
}

static int sceNetApctlScan() {
	ERROR_LOG(SCENET, "UNIMPL %s()", __FUNCTION__);
	return sceNetApctlScanUser();
}

static int sceNetApctlGetBSSDescIDList(u32 sizeAddr, u32 bufAddr) {
	ERROR_LOG(SCENET, "UNIMPL %s(%08x, %08x)", __FUNCTION__, sizeAddr, bufAddr);
	return sceNetApctlGetBSSDescIDListUser(sizeAddr, bufAddr);
}

static int sceNetApctlGetBSSDescEntry(int entryId, int infoId, u32 resultAddr) {
	ERROR_LOG(SCENET, "UNIMPL %s(%i, %i, %08x)", __FUNCTION__, entryId, infoId, resultAddr);
	return sceNetApctlGetBSSDescEntryUser(entryId, infoId, resultAddr);
}

static int sceNetApctl_lib2_C20A144C(int connIndex, u32 ps3MacAddressPtr) {
	ERROR_LOG(SCENET, "UNIMPL %s(%i, %08x)", __FUNCTION__, connIndex, ps3MacAddressPtr);
	return sceNetApctlConnect(connIndex);
}


static int sceNetUpnpInit(int unknown1,int unknown2)
{
	ERROR_LOG_REPORT_ONCE(sceNetUpnpInit, SCENET, "UNIMPLsceNetUpnpInit %d,%d",unknown1,unknown2);	
	return 0;
}

static int sceNetUpnpStart()
{
	ERROR_LOG(SCENET, "UNIMPLsceNetUpnpStart");
	return 0;
}

static int sceNetUpnpStop()
{
	ERROR_LOG(SCENET, "UNIMPLsceNetUpnpStop");
	return 0;
}

static int sceNetUpnpTerm()
{
	ERROR_LOG(SCENET, "UNIMPLsceNetUpnpTerm");
	return 0;
}

static int sceNetUpnpGetNatInfo()
{
	ERROR_LOG(SCENET, "UNIMPLsceNetUpnpGetNatInfo");
	return 0;
}

static int sceNetGetDropRate(u32 dropRateAddr, u32 dropDurationAddr)
{
	Memory::Write_U32(netDropRate, dropRateAddr);
	Memory::Write_U32(netDropDuration, dropDurationAddr);
	return hleLogSuccessInfoI(SCENET, 0);
}

static int sceNetSetDropRate(u32 dropRate, u32 dropDuration)
{
	netDropRate = dropRate;
	netDropDuration = dropDuration;
	return hleLogSuccessInfoI(SCENET, 0);
}

const HLEFunction sceNet[] = {
	{0X39AF39A6, &WrapI_UUUUU<sceNetInit>,           "sceNetInit",                      'i', "xxxxx"},
	{0X281928A9, &WrapU_V<sceNetTerm>,               "sceNetTerm",                      'x', ""     },
	{0X89360950, &WrapV_UU<sceNetEtherNtostr>,       "sceNetEtherNtostr",               'v', "xx"   },
	{0XD27961C9, &WrapV_UU<sceNetEtherStrton>,       "sceNetEtherStrton",               'v', "xx"   },
	{0X0BF0A3AE, &WrapU_U<sceNetGetLocalEtherAddr>,  "sceNetGetLocalEtherAddr",         'x', "x"    },
	{0X50647530, &WrapI_I<sceNetFreeThreadinfo>,     "sceNetFreeThreadinfo",            'i', "i"    },
	{0XCC393E48, &WrapI_U<sceNetGetMallocStat>,      "sceNetGetMallocStat",             'i', "x"    },
	{0XAD6844C6, &WrapI_I<sceNetThreadAbort>,        "sceNetThreadAbort",               'i', "i"    },
};

const HLEFunction sceNetResolver[] = {
	{0X224C5F44, nullptr,                            "sceNetResolverStartNtoA",         '?', ""     },
	{0X244172AF, nullptr,                            "sceNetResolverCreate",            '?', ""     },
	{0X94523E09, nullptr,                            "sceNetResolverDelete",            '?', ""     },
	{0XF3370E61, &WrapI_V<sceNetResolverInit>,       "sceNetResolverInit",              'i', ""     },
	{0X808F6063, nullptr,                            "sceNetResolverStop",              '?', ""     },
	{0X6138194A, nullptr,                            "sceNetResolverTerm",              '?', ""     },
	{0X629E2FB7, nullptr,                            "sceNetResolverStartAtoN",         '?', ""     },
	{0X14C17EF9, nullptr,                            "sceNetResolverStartNtoAAsync",    '?', ""     },
	{0XAAC09184, nullptr,                            "sceNetResolverStartAtoNAsync",    '?', ""     },
	{0X12748EB9, nullptr,                            "sceNetResolverWaitAsync",         '?', ""     },
	{0X4EE99358, nullptr,                            "sceNetResolverPollAsync",         '?', ""     },
};					 

const HLEFunction sceNetInet[] = {
	{0X17943399, &WrapI_V<sceNetInetInit>,           "sceNetInetInit",                  'i', ""     },
	{0X4CFE4E56, nullptr,                            "sceNetInetShutdown",              '?', ""     },
	{0XA9ED66B9, &WrapI_V<sceNetInetTerm>,           "sceNetInetTerm",                  'i', ""     },
	{0X8B7B220F, &WrapI_III<sceNetInetSocket>,       "sceNetInetSocket",                'i', "iii"  },
	{0X2FE71FE7, &WrapI_IIIUI<sceNetInetSetsockopt>, "sceNetInetSetsockopt",            'i', "iiixi"},
	{0X4A114C7C, nullptr,                            "sceNetInetGetsockopt",            '?', ""     },
	{0X410B34AA, &WrapI_IUI<sceNetInetConnect>,      "sceNetInetConnect",               'i', "ixi"  },
	{0X805502DD, nullptr,                            "sceNetInetCloseWithRST",          '?', ""     },
	{0XD10A1A7A, nullptr,                            "sceNetInetListen",                '?', ""     },
	{0XDB094E1B, nullptr,                            "sceNetInetAccept",                '?', ""     },
	{0XFAABB1DD, &WrapI_VUI<sceNetInetPoll>,         "sceNetInetPoll",                  'i', "pxi"  },
	{0X5BE8D595, nullptr,                            "sceNetInetSelect",                '?', ""     },
	{0X8D7284EA, nullptr,                            "sceNetInetClose",                 '?', ""     },
	{0XCDA85C99, &WrapI_IUUU<sceNetInetRecv>,        "sceNetInetRecv",                  'i', "ixxx" },
	{0XC91142E4, nullptr,                            "sceNetInetRecvfrom",              '?', ""     },
	{0XEECE61D2, nullptr,                            "sceNetInetRecvmsg",               '?', ""     },
	{0X7AA671BC, &WrapI_IUUU<sceNetInetSend>,        "sceNetInetSend",                  'i', "ixxx" },
	{0X05038FC7, nullptr,                            "sceNetInetSendto",                '?', ""     },
	{0X774E36F4, nullptr,                            "sceNetInetSendmsg",               '?', ""     },
	{0XFBABE411, &WrapI_V<sceNetInetGetErrno>,       "sceNetInetGetErrno",              'i', ""     },
	{0X1A33F9AE, nullptr,                            "sceNetInetBind",                  '?', ""     },
	{0XB75D5B0A, nullptr,                            "sceNetInetInetAddr",              '?', ""     },
	{0X1BDF5D13, &WrapI_CU<sceNetInetInetAton>,      "sceNetInetInetAton",              'i', "sx"   },
	{0XD0792666, nullptr,                            "sceNetInetInetNtop",              '?', ""     },
	{0XE30B8C19, nullptr,                            "sceNetInetInetPton",              '?', ""     },
	{0X8CA3A97E, nullptr,                            "sceNetInetGetPspError",           '?', ""     },
	{0XE247B6D6, nullptr,                            "sceNetInetGetpeername",           '?', ""     },
	{0X162E6FD5, nullptr,                            "sceNetInetGetsockname",           '?', ""     },
	{0X80A21ABD, nullptr,                            "sceNetInetSocketAbort",           '?', ""     },
	{0X39B0C7D3, nullptr,                            "sceNetInetGetUdpcbstat",          '?', ""     },
	{0XB3888AD4, nullptr,                            "sceNetInetGetTcpcbstat",          '?', ""     },
};

const HLEFunction sceNetApctl[] = {
	{0XCFB957C6, &WrapI_I<sceNetApctlConnect>,       "sceNetApctlConnect",              'i', "i"    },
	{0X24FE91A1, &WrapI_V<sceNetApctlDisconnect>,    "sceNetApctlDisconnect",           'i', ""     },
	{0X5DEAC81B, &WrapI_U<sceNetApctlGetState>,      "sceNetApctlGetState",             'i', "x"    },
	{0X8ABADD51, &WrapU_UU<sceNetApctlAddHandler>,   "sceNetApctlAddHandler",           'x', "xx"   },
	{0XE2F91F9B, &WrapI_II<sceNetApctlInit>,          "sceNetApctlInit",                'i', "ii"   },
	{0X5963991B, &WrapI_U<sceNetApctlDelHandler>,    "sceNetApctlDelHandler",           'i', "x"    },
	{0XB3EDD0EC, &WrapI_V<sceNetApctlTerm>,          "sceNetApctlTerm",                 'i', ""     },
	{0X2BEFDF23, &WrapI_IU<sceNetApctlGetInfo>,      "sceNetApctlGetInfo",              'i', "ix"   },
	{0XA3E77E13, &WrapI_V<sceNetApctlScanSSID2>,     "sceNetApctlScanSSID2",            'i', ""     },
	{0XE9B2E5E6, &WrapI_V<sceNetApctlScanUser>,                 "sceNetApctlScanUser",             'i', ""     },
	{0XF25A5006, &WrapI_UUUU<sceNetApctlGetBSSDescIDList2>,     "sceNetApctlGetBSSDescIDList2",    'i', "xxxx" },
	{0X2935C45B, &WrapI_UUUU<sceNetApctlGetBSSDescEntry2>,      "sceNetApctlGetBSSDescEntry2",     'i', "xxxx" },
	{0X04776994, &WrapI_IIU<sceNetApctlGetBSSDescEntryUser>,    "sceNetApctlGetBSSDescEntryUser",  'i', "iix"  },
	{0X6BDDCB8C, &WrapI_UU<sceNetApctlGetBSSDescIDListUser>,    "sceNetApctlGetBSSDescIDListUser", 'i', "xx"   },
	{0X7CFAB990, &WrapI_UU<sceNetApctlAddInternalHandler>,      "sceNetApctlAddInternalHandler",   'i', "xx"   },
	{0XE11BAFAB, &WrapI_U<sceNetApctlDelInternalHandler>,       "sceNetApctlDelInternalHandler",   'i', "x"    },
	{0XA7BB73DF, &WrapI_UU<sceNetApctl_A7BB73DF>,               "sceNetApctl_A7BB73DF",            'i', "xx"   },
	{0X6F5D2981, &WrapI_U<sceNetApctl_6F5D2981>,                "sceNetApctl_6F5D2981",            'i', "x"    },
	{0X69745F0A, &WrapI_I<sceNetApctl_lib2_69745F0A>,           "sceNetApctl_lib2_69745F0A",       'i', "i"    },
	{0X4C19731F, &WrapI_IU<sceNetApctl_lib2_4C19731F>,          "sceNetApctl_lib2_4C19731F",       'i', "ix"   },
	{0XB3CF6849, &WrapI_V<sceNetApctlScan>,                     "sceNetApctlScan",                 'i', ""     },
	{0X0C7FFA5C, &WrapI_UU<sceNetApctlGetBSSDescIDList>,        "sceNetApctlGetBSSDescIDList",     'i', "xx"   },
	{0X96BEB231, &WrapI_IIU<sceNetApctlGetBSSDescEntry>,        "sceNetApctlGetBSSDescEntry",      'i', "iix"  },
	{0XC20A144C, &WrapI_IU<sceNetApctl_lib2_C20A144C>,          "sceNetApctl_lib2_C20A144C",       'i', "ix"    },
};

const HLEFunction sceWlanDrv[] = {
	{0XD7763699, &WrapU_V<sceWlanGetSwitchState>,    "sceWlanGetSwitchState",           'x', ""     },
	{0X0C622081, &WrapU_U<sceWlanGetEtherAddr>,      "sceWlanGetEtherAddr",             'x', "x"    },
	{0X93440B11, &WrapU_V<sceWlanDevIsPowerOn>,      "sceWlanDevIsPowerOn",             'x', ""     },
};

// see http://www.kingx.de/forum/showthread.php?tid=35164
const HLEFunction sceNetUpnp[] = {
	{0X27045362, &WrapI_V<sceNetUpnpGetNatInfo>,     "sceNetUpnpGetNatInfo",            'i', ""     },
	{0X3432B2E5, &WrapI_V<sceNetUpnpStart>,          "sceNetUpnpStart",                 'i', ""     },
	{0X3E32ED9E, &WrapI_V<sceNetUpnpStop>,           "sceNetUpnpStop",                  'i', ""     },
	{0X540491EF, &WrapI_V<sceNetUpnpTerm>,           "sceNetUpnpTerm",                  'i', ""     },
	{0XE24220B5, &WrapI_II<sceNetUpnpInit>,          "sceNetUpnpInit",                  'i', "ii"   },
};

const HLEFunction sceNetIfhandle[] = {
	{0xC80181A2, &WrapI_UU<sceNetGetDropRate>,     "sceNetGetDropRate",                 'i', "pp" },
	{0xFD8585E1, &WrapI_UU<sceNetSetDropRate>,     "sceNetSetDropRate",                 'i', "ii" },
};

void Register_sceNet() {
	RegisterModule("sceNet", ARRAY_SIZE(sceNet), sceNet);
	RegisterModule("sceNetResolver", ARRAY_SIZE(sceNetResolver), sceNetResolver);
	RegisterModule("sceNetInet", ARRAY_SIZE(sceNetInet), sceNetInet);
	RegisterModule("sceNetApctl", ARRAY_SIZE(sceNetApctl), sceNetApctl);
}

void Register_sceWlanDrv() {
	RegisterModule("sceWlanDrv", ARRAY_SIZE(sceWlanDrv), sceWlanDrv);
}

void Register_sceNetUpnp() {
	RegisterModule("sceNetUpnp", ARRAY_SIZE(sceNetUpnp), sceNetUpnp);
}

void Register_sceNetIfhandle() {
	RegisterModule("sceNetIfhandle", ARRAY_SIZE(sceNetIfhandle), sceNetIfhandle);
}
