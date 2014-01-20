#include "global.h"
#include "CProtoshareProcessor.h"
#include "OpenCLObjects.h"
#include <vector>

// miner version string (for pool statistic)
#ifdef __WIN32__
		char* minerVersionString = "jhProtominer v0.2a";
#else
char* minerVersionString = _strdup("jhProtominer v0.2a-Linux");
#endif

minerSettings_t minerSettings = {0};

xptClient_t* xptClient = NULL;
CRITICAL_SECTION cs_xptClient;
volatile uint32 monitorCurrentBlockHeight; // used to notify worker threads of new block data

typedef struct
{
	char* workername;
	char* workerpass;
	char* host;
	sint32 port;
	sint32 numThreads;
	uint32 ptsMemoryMode;
	// GPU / OpenCL options
	GPUALGO gpuAlgo;
	uint32 deviceNum;
	bool listDevices;
	std::vector<int> deviceList;

	// mode option
	uint32 mode;
}commandlineInput_t;

commandlineInput_t commandlineInput;
std::vector<CProtoshareProcessorGPU *> gpu_processors;

static struct  
{
	CRITICAL_SECTION cs_work;
	uint32	algorithm;
	// block data
	uint32	version;
	uint32	height;
	uint32	nBits;
	uint32	nTime;
	uint8	merkleRootOriginal[32]; // used to identify work
	uint8	prevBlockHash[32];
	uint8	target[32];
	uint8	targetShare[32];
	// extra nonce info
	uint8	coinBase1[1024];
	uint8	coinBase2[1024];
	uint16	coinBase1Size;
	uint16	coinBase2Size;
	// transaction hashes
	uint8	txHash[32*4096];
	uint32	txHashCount;
}workDataSource;

uint32 uniqueMerkleSeedGenerator = 0;
uint32 miningStartTime = 0;

void jhProtominer_submitShare(minerProtosharesBlock_t* block)
{
	printf("Share found! (BlockHeight: %d)\n", block->height);
	EnterCriticalSection(&cs_xptClient);
	if( xptClient == NULL )
	{
		printf("Share submission failed - No connection to server\n");
		LeaveCriticalSection(&cs_xptClient);
		return;
	}
	// submit block
	xptShareToSubmit_t* xptShare = (xptShareToSubmit_t*)malloc(sizeof(xptShareToSubmit_t));
	memset(xptShare, 0x00, sizeof(xptShareToSubmit_t));
	xptShare->algorithm = ALGORITHM_PROTOSHARES;
	xptShare->version = block->version;
	xptShare->nTime = block->nTime;
	xptShare->nonce = block->nonce;
	xptShare->nBits = block->nBits;
	xptShare->nBirthdayA = block->birthdayA;
	xptShare->nBirthdayB = block->birthdayB;
	memcpy(xptShare->prevBlockHash, block->prevBlockHash, 32);
	memcpy(xptShare->merkleRoot, block->merkleRoot, 32);
	memcpy(xptShare->merkleRootOriginal, block->merkleRootOriginal, 32);
	//userExtraNonceLength = min(userExtraNonceLength, 16);
	sint32 userExtraNonceLength = sizeof(uint32);
	uint8* userExtraNonceData = (uint8*)&block->uniqueMerkleSeed;
	xptShare->userExtraNonceLength = userExtraNonceLength;
	memcpy(xptShare->userExtraNonceData, userExtraNonceData, userExtraNonceLength);
	xptClient_foundShare(xptClient, xptShare);
	LeaveCriticalSection(&cs_xptClient);
}

int jhProtominer_minerThread(int threadIndex)
{
	// inits processor
	CProtoshareProcessorGPU *processor = gpu_processors.back();
	gpu_processors.pop_back();
	// pop_back since i'm not sure i can guarantee threadIndex to mean something

	while( true )
	{
		// local work data
		minerProtosharesBlock_t minerProtosharesBlock = {0};
		// has work?
		bool hasValidWork = false;
		EnterCriticalSection(&workDataSource.cs_work);
		if( workDataSource.height > 0 )
		{
			// get work data
			minerProtosharesBlock.version = workDataSource.version;
			//minerProtosharesBlock.nTime = workDataSource.nTime;
			minerProtosharesBlock.nTime = (uint32)time(NULL);
			minerProtosharesBlock.nBits = workDataSource.nBits;
			minerProtosharesBlock.nonce = 0;
			minerProtosharesBlock.height = workDataSource.height;
			memcpy(minerProtosharesBlock.merkleRootOriginal, workDataSource.merkleRootOriginal, 32);
			memcpy(minerProtosharesBlock.prevBlockHash, workDataSource.prevBlockHash, 32);
			memcpy(minerProtosharesBlock.targetShare, workDataSource.targetShare, 32);
			minerProtosharesBlock.uniqueMerkleSeed = uniqueMerkleSeedGenerator;
			uniqueMerkleSeedGenerator++;
			// generate merkle root transaction
			bitclient_generateTxHash(sizeof(uint32), (uint8*)&minerProtosharesBlock.uniqueMerkleSeed, workDataSource.coinBase1Size, workDataSource.coinBase1, workDataSource.coinBase2Size, workDataSource.coinBase2, workDataSource.txHash);
			bitclient_calculateMerkleRoot(workDataSource.txHash, workDataSource.txHashCount+1, minerProtosharesBlock.merkleRoot);
			hasValidWork = true;
		}
		LeaveCriticalSection(&workDataSource.cs_work);
		if( hasValidWork == false )
		{
			Sleep(1);
			continue;
		}
		// valid work data present, start mining
		processor->protoshares_process(&minerProtosharesBlock);
	}
	return 0;
}


/*
 * Reads data from the xpt connection state and writes it to the universal workDataSource struct
 */
void jhProtominer_getWorkFromXPTConnection(xptClient_t* xptClient)
{
	EnterCriticalSection(&workDataSource.cs_work);
	workDataSource.version = xptClient->blockWorkInfo.version;
	//uint32 timeBias = time(NULL) - xptClient->blockWorkInfo.timeWork;
	workDataSource.nTime = xptClient->blockWorkInfo.nTime;// + timeBias;
	workDataSource.nBits = xptClient->blockWorkInfo.nBits;
	memcpy(workDataSource.merkleRootOriginal, xptClient->blockWorkInfo.merkleRoot, 32);
	memcpy(workDataSource.prevBlockHash, xptClient->blockWorkInfo.prevBlockHash, 32);
	memcpy(workDataSource.target, xptClient->blockWorkInfo.target, 32);
	memcpy(workDataSource.targetShare, xptClient->blockWorkInfo.targetShare, 32);

	workDataSource.coinBase1Size = xptClient->blockWorkInfo.coinBase1Size;
	workDataSource.coinBase2Size = xptClient->blockWorkInfo.coinBase2Size;
	memcpy(workDataSource.coinBase1, xptClient->blockWorkInfo.coinBase1, xptClient->blockWorkInfo.coinBase1Size);
	memcpy(workDataSource.coinBase2, xptClient->blockWorkInfo.coinBase2, xptClient->blockWorkInfo.coinBase2Size);

	// get hashes
	if( xptClient->blockWorkInfo.txHashCount >= 256 )
	{
		printf("Too many transaction hashes\n"); 
		workDataSource.txHashCount = 0;
	}
	else
		workDataSource.txHashCount = xptClient->blockWorkInfo.txHashCount;
	for(uint32 i=0; i<xptClient->blockWorkInfo.txHashCount; i++)
		memcpy(workDataSource.txHash+32*(i+1), xptClient->blockWorkInfo.txHashes+32*i, 32);
	// set height last because it is used to detect new work
	workDataSource.height = xptClient->blockWorkInfo.height;
	LeaveCriticalSection(&workDataSource.cs_work);
	monitorCurrentBlockHeight = workDataSource.height;
}

#define getFeeFromFloat(_x) ((uint16)((float)(_x)/0.002f)) // integer 1 = 0.002%

/*
 * Initiates a new xpt connection object and sets up developer fee
 * The new object will be in disconnected state until xptClient_connect() is called
 */
xptClient_t* jhProtominer_initateNewXptConnectionObject()
{
	xptClient_t* xptClient = xptClient_create();
	if( xptClient == NULL )
		return NULL;
	// set developer fees
	// up to 8 fee entries can be set
	// the fee base is always calculated from 100% of the share value
	// for example if you setup two fee entries with 3% and 2%, the total subtracted share value will be 5%
	xptClient_addDeveloperFeeEntry(xptClient, "PkyeQNn1yGV5psGeZ4sDu6nz2vWHTujf4h", getFeeFromFloat(2.5f)); // 0.5% fee (jh00, for testing)
	return xptClient;
}

void jhProtominer_xptQueryWorkLoop()
{
	// init xpt connection object once
	xptClient = jhProtominer_initateNewXptConnectionObject();
	uint32 timerPrintDetails = GetTickCount() + 8000;
	while( true )
	{
		uint32 currentTick = GetTickCount();
		if( currentTick >= timerPrintDetails )
		{
			// print details only when connected
			if( xptClient_isDisconnected(xptClient, NULL) == false )
			{
				uint32 passedSeconds = (uint32)time(NULL) - miningStartTime;
				double collisionsPerMinute = 0.0;
				if( passedSeconds > 5 )
				{
					collisionsPerMinute = (double)totalCollisionCount / (double)passedSeconds * 60.0;
				}
				printf("collisions/min: %.4lf Shares total: %d\n", collisionsPerMinute, totalShareCount);
			}
			timerPrintDetails = currentTick + 8000;
		}
		// check stats
		if( xptClient_isDisconnected(xptClient, NULL) == false )
		{
			EnterCriticalSection(&cs_xptClient);
			xptClient_process(xptClient);
			if( xptClient->disconnected )
			{
				// mark work as invalid
				EnterCriticalSection(&workDataSource.cs_work);
				workDataSource.height = 0;
				monitorCurrentBlockHeight = 0;
				LeaveCriticalSection(&workDataSource.cs_work);
				// we lost connection :(
				printf("Connection to server lost - Reconnect in 45 seconds\n");
				xptClient_forceDisconnect(xptClient);
				LeaveCriticalSection(&cs_xptClient);
				// pause 45 seconds
				Sleep(45000);
			}
			else
			{
				// is Protoshares algorithm?
				if( xptClient->clientState == XPT_CLIENT_STATE_LOGGED_IN && xptClient->algorithm != ALGORITHM_PROTOSHARES )
				{
					printf("The miner is configured to use a different algorithm.\n");
					printf("Make sure your miner login details are correct\n");
					// force disconnect
					xptClient_forceDisconnect(xptClient);
					LeaveCriticalSection(&cs_xptClient);
					// pause 45 seconds
					Sleep(45000);
				}
				else if( xptClient->blockWorkInfo.height != workDataSource.height || memcmp(xptClient->blockWorkInfo.merkleRoot, workDataSource.merkleRootOriginal, 32) != 0 )
				{
					// update work
					jhProtominer_getWorkFromXPTConnection(xptClient);
					LeaveCriticalSection(&cs_xptClient);
				}
				else
					LeaveCriticalSection(&cs_xptClient);
				Sleep(1);
			}
		}
		else
		{
			// initiate new connection
			EnterCriticalSection(&cs_xptClient);
			if( xptClient_connect(xptClient, &minerSettings.requestTarget) == false )
			{
				LeaveCriticalSection(&cs_xptClient);
				printf("Connection attempt failed, retry in 45 seconds\n");
				Sleep(45000);
			}
			else
			{
				LeaveCriticalSection(&cs_xptClient);
				printf("Connected to server using x.pushthrough(xpt) protocol\n");
				miningStartTime = (uint32)time(NULL);
				totalCollisionCount = 0;
			}
			Sleep(1);
		}
	}
}

void jhProtominer_printHelp()
{
	puts("Usage: jhProtominer.exe [options]");
	puts("Options:");
	puts("   -o, -O                The miner will connect to this url");
	puts("                         You can specifiy an port after the url using -o url:port");
	puts("   -u                    The username (workername) used for login");
	puts("   -p                    The password used for login");
	puts("   -d <num,num,num,...>  List of the devices to use (one thread is launched ");
	puts("                         per device, default 0)");
	puts("   -m<amount>            Defines how many megabytes of memory are used per thread.");
	puts("                         Default is 256mb, allowed constants are:");
	puts("                         -m2048 -m1024 -m512 -m256 -m128 -m32 -m8");
	puts("                         Some algorithms, like gpuv4, might consume");
	puts("                         an extra amount of fixed memory. (512Mb extra for gpuv4)");
	puts("   -a <gpuvX>            select the GPU algorithm to use. (default gpuv4)");
	puts("                         valid values are: gpuv2, gpuv3 and gpuv4.");
	puts("Example usage:");
	puts("   jhProtominer.exe -o http://poolurl.com:10034 -u workername.pts_1 -p workerpass -d 0");
}

void jhProtominer_parseCommandline(int argc, char **argv)
{
	sint32 cIdx = 1;
	while( cIdx < argc )
	{
		char* argument = argv[cIdx];
		cIdx++;
		if( memcmp(argument, "-o", 3)==0 || memcmp(argument, "-O", 3)==0 )
		{
			// -o
			if( cIdx >= argc )
			{
				printf("Missing URL after -o option\n");
				exit(0);
			}
			if( strstr(argv[cIdx], "http://") )
				commandlineInput.host = _strdup(strstr(argv[cIdx], "http://")+7);
			else
				commandlineInput.host = _strdup(argv[cIdx]);
			char* portStr = strstr(commandlineInput.host, ":");
			if( portStr )
			{
				*portStr = '\0';
				commandlineInput.port = atoi(portStr+1);
			}
			cIdx++;
		}
		else if( memcmp(argument, "-u", 3)==0 )
		{
			// -u
			if( cIdx >= argc )
			{
				printf("Missing username/workername after -u option\n");
				exit(0);
			}
			commandlineInput.workername = _strdup(argv[cIdx]);
			cIdx++;
		}
		else if( memcmp(argument, "-p", 3)==0 )
		{
			// -p
			if( cIdx >= argc )
			{
				printf("Missing password after -p option\n");
				exit(0);
			}
			commandlineInput.workerpass = _strdup(argv[cIdx]);
			cIdx++;
		}
		else if( memcmp(argument, "-device", 8)==0 || memcmp(argument, "-d", 3)==0 || memcmp(argument, "-devices", 9)==0)
		{
			// -d
			if( cIdx >= argc )
			{
				printf("Missing device list after %s option\n", argument);
				exit(0);
			}
			std::string list = std::string(argv[cIdx]);
			std::string delimiter = ",";
			size_t pos = 0;
			while ((pos = list.find(delimiter)) != std::string::npos) {
				std::string token = list.substr(0, pos);
				commandlineInput.deviceList.push_back(atoi(token.c_str()));
			    list.erase(0, pos + delimiter.length());
			}
			commandlineInput.deviceList.push_back(atoi(list.c_str()));
			cIdx++;
		}
		else if( memcmp(argument, "-algo", 6)==0 || memcmp(argument, "-a", 3)==0 || memcmp(argument, "-gpu", 5)==0)
		{
			// -a
			if( cIdx >= argc )
			{
				printf("Missing algorithm after %s option\n", argument);
				exit(0);
			}
			char* param = argv[cIdx];
			if (memcmp(param, "gpuv2", 6)==0) {
				commandlineInput.gpuAlgo = GPUV2;
			} else if (memcmp(param, "gpuv3", 6)==0) {
				commandlineInput.gpuAlgo = GPUV3;
			} else if (memcmp(param, "gpuv4", 6)==0) {
				commandlineInput.gpuAlgo = GPUV4;
			} else if (memcmp(param, "gpuv5", 6)==0) {
				commandlineInput.gpuAlgo = GPUV5;
			} else {
				printf("Invalid algorithm: %s\n", param);
				exit(0);
			}
			cIdx++;
		}
		else if( memcmp(argument, "-m4096", 7)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_4096;
		}
		else if( memcmp(argument, "-m2048", 7)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_2048;
		}
		else if( memcmp(argument, "-m1024", 7)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_1024;
		}
		else if( memcmp(argument, "-m512", 6)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_512;
		}
		else if( memcmp(argument, "-m256", 6)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_256;
		}
		else if( memcmp(argument, "-m128", 6)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_128;
		}
		else if( memcmp(argument, "-m32", 5)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_32;
		}
		else if( memcmp(argument, "-m8", 4)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_8;
		}
		else if( memcmp(argument, "-list-devices", 14)==0 )
		{
			commandlineInput.listDevices = true;
		}
		else if( memcmp(argument, "-help", 6)==0 || memcmp(argument, "--help", 7)==0 )
		{
			jhProtominer_printHelp();
			exit(0);
		}
		else
		{
			printf("'%s' is an unknown option.\nType jhPrimeminer.exe --help for more info\n", argument); 
			exit(-1);
		}
	}
	if( argc <= 1 )
	{
		jhProtominer_printHelp();
		exit(0);
	}
}


int main(int argc, char** argv)
{
	commandlineInput.host = "ypool.net";
	srand(GetTickCount());
	commandlineInput.port = 8080 + (rand()%8); // use random port between 8080 and 8088
	commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_256;
	SYSTEM_INFO sysinfo;
	GetSystemInfo( &sysinfo );
	commandlineInput.numThreads = 1;
	commandlineInput.deviceNum = 0;
	commandlineInput.gpuAlgo = GPUV4;
	commandlineInput.listDevices = false;
	commandlineInput.deviceList.clear();
	jhProtominer_parseCommandline(argc, argv);
	minerSettings.protoshareMemoryMode = commandlineInput.ptsMemoryMode;
	printf("\xC9\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBB\n");
	printf("\xBA  jhProtominer (v0.2a) + OpenCL GPU Support       \xBA\n");
	printf("\xBA  author: girino, based on code by jh             \xBA\n");
	printf("\xBA                                                  \xBA\n");
	printf("\xBA  If you like it, please donate:                  \xBA\n");
	printf("\xBA  PTS: PkyeQNn1yGV5psGeZ4sDu6nz2vWHTujf4h         \xBA\n");
	printf("\xBA  BTC: 1GiRiNoKznfGbt8bkU1Ley85TgVV7ZTXce         \xBA\n");
	printf("\xBA                                                  \xBA\n");
	printf("\xC8\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBC\n");

	if (commandlineInput.listDevices) {
		printf("Available devices:\n");
		OpenCLMain::getInstance().listDevices();
		exit(0);
	}
	printf("Launching miner...\n");
	size_t mbs = ((1<<commandlineInput.ptsMemoryMode)*sizeof(uint32_t))/(1024*1024);
	if (commandlineInput.gpuAlgo == GPUV4) {
		mbs += (MAX_MOMENTUM_NONCE*sizeof(uint64_t))/(1024*1024);
	}
	printf("Using %ld megabytes of memory per thread\n", mbs);
	printf("Using %d threads\n", commandlineInput.numThreads);

	printf("Available devices:\n");
	OpenCLMain::getInstance().listDevices();
	if (commandlineInput.deviceList.empty()) {
		for (int i = 0; i < commandlineInput.numThreads; i++) {
			commandlineInput.deviceList.push_back(i);
		}
	} else {
		commandlineInput.numThreads = commandlineInput.deviceList.size();
	}
	printf("Adjusting num threads to match device list: %d\n", commandlineInput.numThreads);

	// inits all GPU devices
	printf("Initializing GPU...\n");
	for (int i = 0; i < commandlineInput.deviceList.size(); i++) {
		printf("Initing device %d.\n", i);
		gpu_processors.push_back(new CProtoshareProcessorGPU(commandlineInput.gpuAlgo, commandlineInput.ptsMemoryMode, i, commandlineInput.deviceList[i]));
		printf("Device %d Inited.\n", i);
	}
	printf("All GPUs Initialized...\n");

	// set priority to below normal
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
	// init winsock
	WSADATA wsa;
	WSAStartup(MAKEWORD(2,2),&wsa);
	// get IP of pool url (default ypool.net)
	char* poolURL = commandlineInput.host;//"ypool.net";
	hostent* hostInfo = gethostbyname(poolURL);
	if( hostInfo == NULL )
	{
		printf("Cannot resolve '%s'. Is it a valid URL?\n", poolURL);
		exit(-1);
	}
	void** ipListPtr = (void**)hostInfo->h_addr_list;
	uint32 ip = 0xFFFFFFFF;
	if( ipListPtr[0] )
	{
		ip = *(uint32*)ipListPtr[0];
	}
	char* ipText = (char*)malloc(32);
	sprintf(ipText, "%d.%d.%d.%d", ((ip>>0)&0xFF), ((ip>>8)&0xFF), ((ip>>16)&0xFF), ((ip>>24)&0xFF));
	// init work source
	InitializeCriticalSection(&workDataSource.cs_work);
	InitializeCriticalSection(&cs_xptClient);
	// setup connection info
	minerSettings.requestTarget.ip = ipText;
	minerSettings.requestTarget.port = commandlineInput.port;
	minerSettings.requestTarget.authUser = commandlineInput.workername;//"jh00.pts_1";
	minerSettings.requestTarget.authPass = commandlineInput.workerpass;//"x";
	// start miner threads
	for(uint32 i=0; i<commandlineInput.numThreads; i++)
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);
	/*CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);*/
	// enter work management loop
	jhProtominer_xptQueryWorkLoop();
	return 0;
}
