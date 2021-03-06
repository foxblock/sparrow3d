 /* This file is part of sparrow3d.
  * Sparrow3d is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 2 of the License, or
  * (at your option) any later version.
  * 
  * Sparrow3d is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  * 
  * You should have received a copy of the GNU General Public License
  * along with Foobar.  If not, see <http://www.gnu.org/licenses/>
  * 
  * For feedback and questions about my Files and Projects please mail me,
  * Alexander Matthes (Ziz) , zizsdl_at_googlemail.com */

#include "sparrowNet.h"
#include <stdio.h>
#include <errno.h>

//This is a copy of spReadOneLine sparrowFile. However, I don't want the
//extra dependency of libSparrow3d or linking sparrowFile twice.
int internal_spNet_spReadOneLine( SDL_RWops* file , char* buffer, int buffer_len)
{
	int pos = 0;
	buffer[pos] = 0;
	while (pos < buffer_len)
	{
		if (SDL_RWread( file, &(buffer[pos]), 1, 1 ) <= 0)
			return 1; //EOF
		if ( buffer[pos] == '\n' )
			break;
		if (buffer[pos] != '\r') //fucking windows line break
			pos++;
	}
	buffer[pos] = 0;
	return 0; //not EOF
}

spNetC4ATaskPointer spGlobalC4ATask = NULL;

spNetC4ATaskPointer createNewC4ATask()
{
	spNetC4ATaskPointer task = (spNetC4ATaskPointer)malloc(sizeof(spNetC4ATask));
	task->statusMutex = SDL_CreateMutex();
	task->status = 0;
	task->dataPointer = NULL;
	task->timeOut = 0;
	task->thread = NULL;
	task->result = 0;
	task->threadStatus = 0;
	return task;
}

void spNetC4ADeleteTask(spNetC4ATaskPointer task)
{
	SDL_DestroyMutex(task->statusMutex);
	free(task);
}

PREFIX void spInitNet()
{
	if(SDLNet_Init()==-1)
		printf("SDLNet_Init: %s\n", SDLNet_GetError());
	spGlobalC4ATask = createNewC4ATask();
}

void fill_ip_struct(spNetIPPointer ip)
{
	ip->type = IPV4;
	ip->address.ipv4 = ip->sdl_address.host; //bytes are set automaticly, yeah!
	ip->port = ip->sdl_address.port;	
}

PREFIX spNetIP spNetResolve(char* host,Uint16 port)
{
	spNetIP result;
	SDLNet_ResolveHost(&(result.sdl_address), host, port);
	fill_ip_struct(&result);
	return result;
}

PREFIX char* spNetResolveHost(spNetIP ip,char* host,int host_len)
{
	const char* sdlHost = SDLNet_ResolveIP(&(ip.sdl_address));
	if (strlen(sdlHost) >= host_len)
	{
		host = NULL;
		return NULL;
	}
	sprintf(host,"%s",sdlHost);
	return host;
}

PREFIX spNetTCPConnection spNetOpenClientTCP(spNetIP ip)
{
	spNetTCPConnection result;

	result=SDLNet_TCP_Open(&(ip.sdl_address));
	if(!result) {
		printf("SDLNet_TCP_Open: %s\n", SDLNet_GetError());
		return NULL;
	}
	return result;
}

PREFIX spNetTCPServer spNetOpenServerTCP(Uint16 port)
{
	IPaddress ip;
	spNetTCPServer result;

	if(SDLNet_ResolveHost(&ip,NULL,port)==-1) {
		printf("SDLNet_ResolveHost: %s\n", SDLNet_GetError());
		return NULL;
	}
	result=SDLNet_TCP_Open(&ip);
	if(!result) {
		printf("SDLNet_TCP_Open: %s\n", SDLNet_GetError());
		return NULL;
	}
	return result;
}

PREFIX spNetTCPConnection spNetAcceptTCP(spNetTCPServer server)
{
	return SDLNet_TCP_Accept(server);
}

PREFIX spNetIP spNetGetConnectionIP(spNetTCPConnection connection)
{
	IPaddress *temp_ip;
	temp_ip=SDLNet_TCP_GetPeerAddress(connection);
	spNetIP result;
	if(!temp_ip) {
		printf("SDLNet_TCP_GetPeerAddress: %s\n", SDLNet_GetError());
		printf("This may be a server socket.\n");
		printf("However, the ip may not be valid!\n");
	}
	result.sdl_address = *temp_ip;
	fill_ip_struct(&result);
	return result;
}

PREFIX int spNetSendTCP(spNetTCPConnection connection,void* data,int length)
{
	return SDLNet_TCP_Send(connection,data,length);
}

PREFIX int spNetSendHTTP(spNetTCPConnection connection,char* data)
{
	return spNetSendTCP(connection,data,strlen(data)+1);
}

typedef struct receivingStruct *receivingPointer;
typedef struct receivingStruct {
	spNetTCPConnection connection;
	void* data;
	int length;
	SDL_mutex* mutex;
	int done;
	SDL_Thread* thread;
	int result;
	receivingPointer next;
} receivingType;

int tcpReceiveThread(void* data)
{
	receivingPointer tcpData = (receivingPointer)data;
	int res=spNetReceiveTCP(tcpData->connection,tcpData->data,tcpData->length);
	SDL_mutexP(tcpData->mutex);
	tcpData->done = 1;
	tcpData->result = res;
	SDL_mutexV(tcpData->mutex);
	return res;
}

int tcpReceiveThread_http(void* data)
{
	receivingPointer tcpData = (receivingPointer)data;
	int res=spNetReceiveHTTP(tcpData->connection,(char*)tcpData->data,tcpData->length);
	SDL_mutexP(tcpData->mutex);
	tcpData->done = 1;
	tcpData->result = res;
	SDL_mutexV(tcpData->mutex);
	return res;
}

receivingPointer firstReceiving = NULL;

SDL_Thread* allreadyReceiving(spNetTCPConnection connection)
{
	receivingPointer before = NULL;
	receivingPointer mom = firstReceiving;
	while (mom)
	{
		if (mom->connection == connection)
		{
			SDL_mutexP(mom->mutex);
			if (mom->done)
			{
				SDL_mutexV(mom->mutex); //The Thread lost the interest on this struct
				//Removing mom
				if (before)
				{
					SDL_mutexP(before->mutex);
					before->next = mom->next;
					SDL_mutexV(before->mutex);
				}
				else
					firstReceiving = mom->next;
				SDL_DestroyMutex(mom->mutex);
				if (mom->result<=0) //connection destroyed!
				{
					free(mom);
					return (SDL_Thread*)(-1);
				}
				free(mom);
				return NULL;
			}
			SDL_mutexV(mom->mutex);
			return mom->thread;
		}
		before = mom;
		mom = mom->next;
	}	
	return NULL;
}

PREFIX int spNetReceiveTCP(spNetTCPConnection connection,void* data,int length)
{
	char* data_pointer = (char*)data;
	return SDLNet_TCP_Recv(connection,&(data_pointer[0]),length);
}

PREFIX SDL_Thread* spNetReceiveTCPUnblocked(spNetTCPConnection connection,void* data,int length)
{
	SDL_Thread* thread;
	if (thread = allreadyReceiving(connection))
		return thread;
	receivingPointer tcpData = (receivingPointer)malloc(sizeof(receivingType));
	tcpData->connection = connection;
	tcpData->data = data;
	tcpData->length = length;
	tcpData->connection = connection;
	tcpData->done = 0;
	tcpData->mutex = SDL_CreateMutex();
	tcpData->next = firstReceiving;
	firstReceiving = tcpData;
	tcpData->thread = SDL_CreateThread(tcpReceiveThread,tcpData);
	return tcpData->thread;
}

PREFIX int spNetReceiveHTTP(spNetTCPConnection connection,char* data,int length)
{
	int received = 0;
	while (length > 0)
	{
		int new_received = spNetReceiveTCP(connection,&(data[received]),length);
		received+=new_received;
		length-=new_received;
		if (new_received == 0)
			return received;
	}
	return received;
}

PREFIX SDL_Thread* spNetReceiveHTTPUnblocked(spNetTCPConnection connection,char* data,int length)
{
	SDL_Thread* thread;
	if (thread = allreadyReceiving(connection))
		return thread;
	receivingPointer tcpData = (receivingPointer)malloc(sizeof(receivingType));
	tcpData->connection = connection;
	tcpData->data = data;
	tcpData->length = length;
	tcpData->connection = connection;
	tcpData->done = 0;
	tcpData->mutex = SDL_CreateMutex();
	tcpData->next = firstReceiving;
	firstReceiving = tcpData;
	tcpData->thread = SDL_CreateThread(tcpReceiveThread_http,tcpData);
	return tcpData->thread;
}

PREFIX int spNetReceiveStillWaiting(SDL_Thread* thread)
{
	receivingPointer before = NULL;
	receivingPointer mom = firstReceiving;
	while (mom)
	{
		if (mom->thread == thread)
		{
			SDL_mutexP(mom->mutex);
			if (mom->done)
			{
				SDL_mutexV(mom->mutex); //The Thread lost the interest on this struct
				//Removing mom
				if (before)
				{
					SDL_mutexP(before->mutex);
					before->next = mom->next;
					SDL_mutexV(before->mutex);
				}
				else
					firstReceiving = mom->next;
				SDL_DestroyMutex(mom->mutex);
				free(mom);
				return 0;
			}
			SDL_mutexV(mom->mutex);
			return 1;
		}
		before = mom;
		mom = mom->next;
	}	
	return 0;
}

PREFIX void spNetCloseTCP(spNetTCPConnection connection)
{
	SDLNet_TCP_Close(connection);
}

PREFIX void spQuitNet()
{
	spNetC4ADeleteTask(spGlobalC4ATask);
	spGlobalC4ATask = NULL;
	SDLNet_Quit();
}

#ifdef PANDORA
	#include "pnd_locate.h"
#endif

typedef struct getgenericStruct *getgenericPointer;
typedef struct getgenericStruct {
	spNetC4ATaskPointer task;
	int ( *function )( void* data );
} getgenericType;

typedef struct getgameStruct *getgamePointer;
typedef struct getgameStruct {
	spNetC4ATaskPointer task;
	int ( *function )( void* data );
	spNetC4AGamePointer* game;
} getgameType;

typedef struct getscoreStruct *getscorePointer;
typedef struct getscoreStruct {
	spNetC4ATaskPointer task;
	int ( *function )( void* data );
	spNetC4AScorePointer* score;
	spNetC4AProfilePointer profile;
	int year;
	int month;
	char game[256];
} getscoreType;

typedef struct commitStruct *commitPointer;
typedef struct commitStruct {
	spNetC4ATaskPointer task;
	int ( *function )( void* data );
	spNetC4AProfilePointer profile;
	char game[256];
	int score;
	char system[256];
	spNetC4AScorePointer* scoreList;
} commitType;

typedef struct createStruct *createPointer;
typedef struct createStruct {
	spNetC4ATaskPointer task;
	int ( *function )( void* data );
	spNetC4AProfilePointer* profile;
	char longname[256];
	char shortname[256];
	char password[256];
	char email[256];
	int deleteFile;
} createType;


//This is usefull for debugging without threading influences:
/*#define SDL_CreateThread SDL_CreateThreadWithoutThreading
SDL_Thread* SDL_CreateThreadWithoutThreading(int (*fn)(void *),void* data)
{
	fn(data);
	return NULL;
}*/

int spNetC4AUberThread(getgenericPointer data)
{
	int startTime = SDL_GetTicks();
	SDL_Thread* thread = SDL_CreateThread(data->function,data);
	while (1)
	{
	#ifdef REALGP2X
		//TODO: Implement!
		SDL_Delay(1);
	#elif defined WIN32
		SDL_Delay(1);	
	#else
		usleep(100);
	#endif
		int newTime = SDL_GetTicks();
		int diff = newTime - startTime;
		startTime = newTime;
		data->task->timeOut -= diff;
		SDL_mutexP(data->task->statusMutex);
		int status = data->task->status;
		SDL_mutexV(data->task->statusMutex);
		if (status == SP_C4A_CANCELED || data->task->timeOut <= 0)
		{
			SDL_KillThread(thread);
			data->task->result = 1;
			SDL_mutexP(data->task->statusMutex);
			if (data->task->timeOut <= 0)
				data->task->status = SP_C4A_TIMEOUT;
			data->task->threadStatus = 0;
			SDL_mutexV(data->task->statusMutex);
			int result = data->task->result;
			data->task->dataPointer = NULL;
			free(data);
			return result;
		}
		if (status <= 0) //finished somehow
		{			
			SDL_WaitThread(thread,&(data->task->result));
			SDL_mutexP(data->task->statusMutex);
			data->task->threadStatus = 0;
			SDL_mutexV(data->task->statusMutex);
			int result = data->task->result;
			data->task->dataPointer = NULL;
			free(data);
			return result;
		}
	}
}

char* my_strchr(char* buffer, char c, char ignore)
{
	int i;
	int in_ignore = 0;
	for (i = 0; buffer[i]!=0; i++)
	{
		if (buffer[i] == ignore)
			in_ignore = 1-in_ignore;
		if (!in_ignore && buffer[i] == c)
			return &(buffer[i]);
	}
	return NULL;
}

void fill_between_paraphrases(char* buffer, char* dest, int max_size)
{
	int i,j = 0;
	int in_paraphrases = 0;
	for (i = 0; buffer[i]!=0; i++)
	{
		if (buffer[i] == '\"')
		{
			switch (in_paraphrases)
			{
				case 0: in_paraphrases = 1; break;
				case 1: dest[j]=0;return;
			}
			continue;
		}
		if (in_paraphrases)
		{
			dest[j] = buffer[i];
			j++;
			if (j == max_size)
			{
				dest[j-1] = 0;
				return;
			}
		}
	}
}

void internal_CreateDirectoryChain( const char* directories)
{
	//Creating copy:
	int len = strlen(directories)+1;
	#ifdef __GNUC__
		char directoriesCopy[len];
	#else
		char* directoriesCopy = (char*)malloc( len * sizeof(char) );
	#endif
	memcpy(directoriesCopy,directories,len);
	//Splitting in subdirectories
	char* subString = directoriesCopy;
	char* endOfString = strchr(subString,'/');
	if (endOfString == NULL)
		endOfString = strchr(subString,0);
	while (endOfString)
	{
		char oldChar = endOfString[0];
		endOfString[0] = 0;
		#ifdef WIN32

			if (CreateDirectory(directoriesCopy,NULL))
			{}
			else
			if (GetLastError() != ERROR_ALREADY_EXISTS)
				break;
		#else
			int error = mkdir(directoriesCopy,0777);
			if (errno == 0 || errno == EEXIST || errno == ENOENT) //thats okay :)
			{}
			else //not okay
				break;
		#endif
		endOfString[0] = oldChar;
		if (oldChar == 0)
			break;
		subString = &(endOfString[1]);
		endOfString = strchr(subString,'/');
		if (endOfString == NULL)
			endOfString = strchr(subString,0);
	}
	#ifndef __GNUC__
		free(directoriesCopy);
	#endif
}

#ifdef PANDORA
	#define PROFILE_FILENAME_MAKRO char *filename = pnd_locate_filename ( "/media/*/pandora/appdata/c4a-mame/:.", "c4a-prof" );
#elif defined GCW || (defined X86CPU && !defined WIN32)
	#define PROFILE_FILENAME_MAKRO char filename[256]; \
		sprintf(filename,"%s/.config/compo4all",getenv("HOME"));\
		internal_CreateDirectoryChain(filename);\
		sprintf(filename,"%s/.config/compo4all/c4a-prof",getenv("HOME"));
#else
	#define PROFILE_FILENAME_MAKRO char filename[256] = "./c4a-prof";
#endif

PREFIX spNetC4AProfilePointer spNetC4AGetProfile()
{
	spNetC4AProfilePointer profile = NULL;
	PROFILE_FILENAME_MAKRO
	//Parsing the file
	SDL_RWops *file=SDL_RWFromFile(filename,"rb");
	if (file == NULL)
		return NULL;
	profile = (spNetC4AProfilePointer)malloc(sizeof(spNetC4AProfile));
	char buffer[2048];
	internal_spNet_spReadOneLine(file,buffer,2048);
	internal_spNet_spReadOneLine(file,buffer,2048);
	char* pos = strstr( buffer, "\"longname\":");
	pos+=11;
	fill_between_paraphrases( pos, profile->longname, 256);
	
	pos = strstr( buffer, "\"shortname\":");
	pos+=12;
	fill_between_paraphrases( pos, profile->shortname, 256);
	
	pos = strstr( buffer, "\"prid\":");
	pos+=7;
	fill_between_paraphrases( pos, profile->prid, 256);

	pos = strstr( buffer, "\"email\":");
	pos+=8;
	fill_between_paraphrases( pos, profile->email, 256);

	pos = strstr( buffer, "\"password\":");
	pos+=11;
	fill_between_paraphrases( pos, profile->password, 256);
	SDL_RWclose(file);
	return profile;
}

PREFIX void spNetC4AFreeProfile(spNetC4AProfilePointer profile)
{
	if (profile)
		free(profile);
}

int c4a_getgame_thread(void* data)
{
	getgamePointer gameData = ((getgamePointer)data);
	spNetIP ip = spNetResolve("skeezix.wallednetworks.com",13001);
	if (ip.address.ipv4 == SP_INVALID_IP)
	{
		SDL_mutexP(gameData->task->statusMutex);
		gameData->task->status = SP_C4A_ERROR;
		SDL_mutexV(gameData->task->statusMutex);
		return 1;
	}	
	spNetTCPConnection connection = spNetOpenClientTCP(ip);
	if (connection == NULL)
	{
		SDL_mutexP(gameData->task->statusMutex);
		gameData->task->status = SP_C4A_ERROR;
		SDL_mutexV(gameData->task->statusMutex);
		return 1;
	}
	char get_string[512] = "GET /curgamelist_1\n\n";
	if (spNetSendHTTP(connection,get_string) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(gameData->task->statusMutex);
		gameData->task->status = SP_C4A_ERROR;
		SDL_mutexV(gameData->task->statusMutex);
		return 1;
	}
	char buffer[50001]; //skeezix saves the top500. So 100 byte should be enough...
	int length;
	if ((length = spNetReceiveHTTP(connection,buffer,50000)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(gameData->task->statusMutex);
		gameData->task->status = SP_C4A_ERROR;
		SDL_mutexV(gameData->task->statusMutex);
		return 1;
	}
	buffer[length] = 0;
	spNetCloseTCP(connection);
	//Searching the first [
	char* found = strchr( buffer, '[' );
	if (found == NULL)
	{
		SDL_mutexP(gameData->task->statusMutex);
		gameData->task->status = SP_C4A_ERROR;
		SDL_mutexV(gameData->task->statusMutex);
		return 1;
	}
	//Reading game by game
	//Searching the starting {
	while (found)
	{
		char* start = strchr( found, '{' );
		if (start == NULL)
		{
			SDL_mutexP(gameData->task->statusMutex);
			gameData->task->status = SP_C4A_ERROR;
			SDL_mutexV(gameData->task->statusMutex);
			return 1;
		}
		char* end = my_strchr( start, '}', '\"'); //ignore "text}"-parts
		if (start == NULL)
		{
			SDL_mutexP(gameData->task->statusMutex);
			gameData->task->status = SP_C4A_ERROR;
			SDL_mutexV(gameData->task->statusMutex);
			return 1;
		}
		//Creating substring:
		end[0] = 0;
		//Now we search in the substring
		//Search for the long name:
		char* pos = strstr( start, "\"longname\":");
		pos+=11;
		char longname[256];
		fill_between_paraphrases( pos, longname, 128);
		
		pos = strstr( start, "\"gamename\":");
		pos+=11;
		char shortname[256];
		fill_between_paraphrases( pos, shortname, 128);
		
		pos = strstr( start, "\"genre\":");
		pos+=8;
		char genre[256];
		fill_between_paraphrases( pos, genre, 128);
		
		pos = strstr( start, "\"field\":");
		pos+=8;
		char field[256];
		fill_between_paraphrases( pos, field, 128);
		
		pos = strstr( start, "\"status\":");
		pos+=9;
		char status[256];
		fill_between_paraphrases( pos, status, 128);
		
		//Re"inserting" substring:
		end[0] = '}';
		found = strchr( end, '{' );
		
		//Adding the found stuff to the array:
		spNetC4AGamePointer new_game = (spNetC4AGamePointer)malloc(sizeof(spNetC4AGame));
		sprintf(new_game->longname,"%s",longname);
		sprintf(new_game->shortname,"%s",shortname);
		sprintf(new_game->genre,"%s",genre);
		if (strcmp(status,"available") == 0)
			new_game->status = 1;
		else
		if (strcmp(status,"active") == 0)
			new_game->status = 2;
		else
			new_game->status = 0;
		if (strcmp(field,"arcade") == 0)
			new_game->field = 1;
		else
		if (strcmp(field,"indie") == 0)
			new_game->field = 0;
		else
			new_game->field = -1;
		
		//sorted insert
		//Searching the next and before element:
		spNetC4AGamePointer before = NULL;
		spNetC4AGamePointer next = *(gameData->game);
		while (next)
		{
			if (strcmp(new_game->longname,next->longname) < 0)
				break;
			before = next;
			next = next->next;
		}
		if (before == NULL) //new first element!
		{
			new_game->next = next;
			(*(gameData->game)) = new_game;
		}
		else
		{
			before->next = new_game;
			new_game->next = next;
		}
	}	
	SDL_mutexP(gameData->task->statusMutex);
	gameData->task->status = SP_C4A_OK;
	SDL_mutexV(gameData->task->statusMutex);
	return 0;
}

PREFIX int spNetC4AGetGame(spNetC4AGamePointer* gameList,int timeOut)
{
	(*gameList) = NULL;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		getgamePointer data = (getgamePointer)malloc(sizeof(getgameType));
		data->function = c4a_getgame_thread;
		data->task = spGlobalC4ATask;
		data->game = gameList;
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,data);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;	
}

PREFIX spNetC4ATaskPointer spNetC4AGetGameParallel(spNetC4AGamePointer* gameList,int timeOut)
{
	(*gameList) = NULL;
	spNetC4ATaskPointer task = createNewC4ATask();
	task->status = SP_C4A_PROGRESS;
	//Starting a background thread, which does the fancy stuff
	getgamePointer data = (getgamePointer)malloc(sizeof(getgameType));
	data->function = c4a_getgame_thread;
	data->task = task;
	data->game = gameList;
	task->dataPointer = data;
	task->timeOut = timeOut;
	task->threadStatus = 1;
	#ifdef _MSC_VER
		task->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
	#else
		task->thread = SDL_CreateThread(spNetC4AUberThread,data);
	#endif
	return task;
}

int c4a_getscore_thread(void* data)
{
	getscorePointer scoreData = (getscorePointer)data;
	spNetIP ip = spNetResolve("skeezix.wallednetworks.com",13001);
	if (ip.address.ipv4 == SP_INVALID_IP)
	{
		SDL_mutexP(scoreData->task->statusMutex);
		scoreData->task->status = SP_C4A_ERROR;
		SDL_mutexV(scoreData->task->statusMutex);
		return 1;
	}	
	spNetTCPConnection connection = spNetOpenClientTCP(ip);
	if (connection == NULL)
	{
		SDL_mutexP(scoreData->task->statusMutex);
		scoreData->task->status = SP_C4A_ERROR;
		SDL_mutexV(scoreData->task->statusMutex);
		return 1;
	}
	char get_string[512];
	if (scoreData->year && scoreData->month)
		sprintf(get_string,"GET /json_1/%s/%i%02i/\n\n",scoreData->game,scoreData->year,scoreData->month);
	else
		sprintf(get_string,"GET /json_1/%s/all\n\n",scoreData->game);
	if (spNetSendHTTP(connection,get_string) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(scoreData->task->statusMutex);
		scoreData->task->status = SP_C4A_ERROR;
		SDL_mutexV(scoreData->task->statusMutex);
		return 1;
	}
	char buffer[50001]; //skeezix saves the top500. So 100 byte should be enough...
	int length;
	if ((length = spNetReceiveHTTP(connection,buffer,50000)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(scoreData->task->statusMutex);
		scoreData->task->status = SP_C4A_ERROR;
		SDL_mutexV(scoreData->task->statusMutex);
		return 1;
	}
	buffer[length] = 0;
	spNetCloseTCP(connection);
	//Searching the first [
	char* found = strchr( buffer, '[' );
	if (found == NULL)
	{
		SDL_mutexP(scoreData->task->statusMutex);
		scoreData->task->status = SP_C4A_ERROR;
		SDL_mutexV(scoreData->task->statusMutex);
		return 1;
	}
	//Reading score by score
	//Searching the starting {
	int rank = 1;
	spNetC4AScorePointer lastScore = NULL;
	while (found)
	{
		char* start = strchr( found, '{' );
		if (start == NULL)
		{
			SDL_mutexP(scoreData->task->statusMutex);
			scoreData->task->status = SP_C4A_ERROR;
			SDL_mutexV(scoreData->task->statusMutex);
			return 1;
		}
		char* end = my_strchr( start, '}', '\"'); //ignore "text}"-parts
		if (start == NULL)
		{
			SDL_mutexP(scoreData->task->statusMutex);
			scoreData->task->status = SP_C4A_ERROR;
			SDL_mutexV(scoreData->task->statusMutex);
			return 1;
		}
		//Creating substring:
		end[0] = 0;
		//Now we search in the substring
		//Search for the long name:
		char* pos = strstr( start, "\"longname\":");
		pos+=11;
		char longname[128];
		fill_between_paraphrases( pos, longname, 128);
		
		pos = strstr( start, "\"shortname\":");
		pos+=12;
		char shortname[128];
		fill_between_paraphrases( pos, shortname, 128);
		
		pos = strstr( start, "\"score\":");
		pos+=8;
		int score = atoi(pos);
		
		pos = strstr( start, "\"time\":");
		pos+=7;
		Uint64 commitTime = (Uint64)(atof(pos)); //float becase of bigger numbers
		
		//Re"inserting" substring:
		end[0] = '}';
		found = strchr( end, '{' );
		
		//Adding the found stuff to the array:
		if (longname[0] == 0 || shortname[0] == 0)
			continue;
		if (scoreData->profile && (strcmp(scoreData->profile->longname,longname) != 0 || strcmp(scoreData->profile->shortname,shortname) != 0))
			continue;
		spNetC4AScorePointer new_score = (spNetC4AScorePointer)malloc(sizeof(spNetC4AScore));
		sprintf(new_score->longname,"%s",longname);
		sprintf(new_score->shortname,"%s",shortname);
		new_score->score = score;
		new_score->commitTime = commitTime;
		new_score->next = NULL;
		new_score->rank = rank;
		if (lastScore == NULL)
			(*(scoreData->score)) = new_score;
		else
			lastScore->next = new_score;
		lastScore = new_score;
		rank++;
	}	
	
	SDL_mutexP(scoreData->task->statusMutex);
	scoreData->task->status = SP_C4A_OK;
	SDL_mutexV(scoreData->task->statusMutex);
	return 0;
}

PREFIX void spNetC4ADeleteGames(spNetC4AGamePointer* gameList)
{
	if (gameList == NULL)
		return;
	while (*gameList)
	{
		spNetC4AGamePointer next = (*gameList)->next;
		free(*gameList);
		(*gameList) = next;
	}
}

PREFIX int spNetC4AGetScore(spNetC4AScorePointer* scoreList,spNetC4AProfilePointer profile,char* game,int timeOut)
{
	(*scoreList) = NULL;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		getscorePointer data = (getscorePointer)malloc(sizeof(getscoreType));
		data->function = c4a_getscore_thread;
		data->task = spGlobalC4ATask;
		data->score = scoreList;
		data->profile = profile;
		data->year = 0;
		data->month = 0;
		sprintf(data->game,"%s",game);
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,data);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;
}

PREFIX int spNetC4AGetScoreOfMonth(spNetC4AScorePointer* scoreList,spNetC4AProfilePointer profile,char* game,int year,int month,int timeOut)
{
	(*scoreList) = NULL;
	if (month < 1 || month > 12)
		return 1;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		getscorePointer data = (getscorePointer)malloc(sizeof(getscoreType));
		data->function = c4a_getscore_thread;
		data->task = spGlobalC4ATask;
		data->score = scoreList;
		data->profile = profile;
		data->year = year;
		data->month = month;
		sprintf(data->game,"%s",game);
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,data);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;
}

PREFIX spNetC4ATaskPointer spNetC4AGetScoreParallel(spNetC4AScorePointer* scoreList,spNetC4AProfilePointer profile,char* game,int timeOut)
{
	(*scoreList) = NULL;
	spNetC4ATaskPointer task = createNewC4ATask();
	task->status = SP_C4A_PROGRESS;
	//Starting a background thread, which does the fancy stuff
	getscorePointer data = (getscorePointer)malloc(sizeof(getscoreType));
	data->function = c4a_getscore_thread;
	data->task = task;
	data->score = scoreList;
	data->profile = profile;
	data->year = 0;
	data->month = 0;
	sprintf(data->game,"%s",game);
	task->dataPointer = data;
	task->timeOut = timeOut;
	task->threadStatus = 1;
	#ifdef _MSC_VER
		task->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
	#else
		task->thread = SDL_CreateThread(spNetC4AUberThread,data);
	#endif
	return task;
}

PREFIX spNetC4ATaskPointer spNetC4AGetScoreOfMonthParallel(spNetC4AScorePointer* scoreList,spNetC4AProfilePointer profile,char* game,int year,int month,int timeOut)
{
	(*scoreList) = NULL;
	if (month < 1 || month > 12)
		return NULL;
	spNetC4ATaskPointer task = createNewC4ATask();
	task->status = SP_C4A_PROGRESS;
	SDL_mutexV(task->statusMutex);
	//Starting a background thread, which does the fancy stuff
	getscorePointer data = (getscorePointer)malloc(sizeof(getscoreType));
	data->function = c4a_getscore_thread;
	data->task = task;
	data->score = scoreList;
	data->profile = profile;
	data->year = year;
	data->month = month;
	sprintf(data->game,"%s",game);
	task->dataPointer = data;
	task->timeOut = timeOut;
	task->threadStatus = 1;
	#ifdef _MSC_VER
		task->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
	#else
		task->thread = SDL_CreateThread(spNetC4AUberThread,data);
	#endif
	return task;
}

int c4a_commit_thread(void* data)
{
	commitPointer commitData = (commitPointer)data;
	spNetIP ip = spNetResolve("skeezix.wallednetworks.com",13001);
	if (ip.address.ipv4 == SP_INVALID_IP)
	{
		SDL_mutexP(commitData->task->statusMutex);
		commitData->task->status = SP_C4A_ERROR;
		SDL_mutexV(commitData->task->statusMutex);
		return 1;
	}	
	spNetTCPConnection connection = spNetOpenClientTCP(ip);
	if (connection == NULL)
	{
		SDL_mutexP(commitData->task->statusMutex);
		commitData->task->status = SP_C4A_ERROR;
		SDL_mutexV(commitData->task->statusMutex);
		return 1;
	}
	char commit_string[2048];
	sprintf(commit_string,"score=%i",commitData->score);
	int dataSize = strlen(commit_string);
	sprintf(commit_string,"PUT /plugtally_1/scoreonly/%s/%s/%s?score=%i HTTP/1.1\r\nHost: skeezix.wallednetworks.com:13001\r\nAccept: */*\r\nContent-Length: 0\r\nExpect: 100-continue\r\n",commitData->game,commitData->system,commitData->profile->prid,commitData->score);
	if (spNetSendHTTP(connection,commit_string) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(commitData->task->statusMutex);
		commitData->task->status = SP_C4A_ERROR;
		SDL_mutexV(commitData->task->statusMutex);
		return 1;
	}
	//printf("Did:\n%s",commit_string);
	spNetCloseTCP(connection);
	//Adding to scoreList ;)
	if (commitData->scoreList)
	{
		spNetC4AScorePointer new_score = (spNetC4AScorePointer)malloc(sizeof(spNetC4AScore));
		sprintf(new_score->longname,"%s",commitData->profile->longname);
		sprintf(new_score->shortname,"%s",commitData->profile->shortname);
		new_score->score = commitData->score;
		new_score->commitTime = time(NULL);
		new_score->next = (*(commitData->scoreList));
		(*(commitData->scoreList)) = new_score;
	}
	SDL_mutexP(commitData->task->statusMutex);
	commitData->task->status = SP_C4A_OK;
	SDL_mutexV(commitData->task->statusMutex);
	return 0;
}

int already_in_highscore(spNetC4AScorePointer scoreList,spNetC4AProfilePointer profile,int score)
{
	if (scoreList == NULL)
		return 0;
	while (scoreList)
	{
		if (strcmp(scoreList->longname,profile->longname) == 0 &&
		    strcmp(scoreList->shortname,profile->shortname) == 0 &&
		    scoreList->score == score)
			return 1;
		scoreList = scoreList->next;
	}
	return 0;
}

PREFIX int spNetC4ACommitScore(spNetC4AProfilePointer profile,char* game,int score,spNetC4AScorePointer* scoreList,int timeOut)
{
	if (profile == NULL)
		return 1;
	if (scoreList && already_in_highscore(*scoreList,profile,score))
		return 1;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		commitPointer data = (commitPointer)malloc(sizeof(commitType));
		data->task = spGlobalC4ATask;
		data->function = c4a_commit_thread;
		data->score = score;
		data->profile = profile;
		data->scoreList = scoreList;
		sprintf(data->game,"%s",game);
		#ifdef GP2X
			sprintf(data->system,"gp2x");
		#elif defined(CAANOO)
			sprintf(data->system,"caanoo");
		#elif defined(WIZ)
			sprintf(data->system,"wiz");
		#elif defined(DINGUX)
				sprintf(data->system,"dingux");
		#elif defined(GCW)	
			sprintf(data->system,"gcw");
		#elif defined(PANDORA)	
			sprintf(data->system,"pandora");
		#elif defined(WIN32)
			sprintf(data->system,"win32");
		#else
			sprintf(data->system,"linux");
		#endif
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,data);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;
}

PREFIX spNetC4ATaskPointer spNetC4ACommitScoreParallel(spNetC4AProfilePointer profile,char* game,int score,spNetC4AScorePointer* scoreList,int timeOut)
{
	if (profile == NULL)
		return NULL;
	if (scoreList && already_in_highscore(*scoreList,profile,score))
		return NULL;
	spNetC4ATaskPointer task = createNewC4ATask();
	task->status = SP_C4A_PROGRESS;
	//Starting a background thread, which does the fancy stuff
	commitPointer data = (commitPointer)malloc(sizeof(commitType));
	data->task = task;
	data->function = c4a_commit_thread;
	data->score = score;
	data->profile = profile;
	data->scoreList = scoreList;
	sprintf(data->game,"%s",game);
	#ifdef GP2X
		sprintf(data->system,"gp2x");
	#elif defined(CAANOO)
		sprintf(data->system,"caanoo");
	#elif defined(WIZ)
		sprintf(data->system,"wiz");
	#elif defined(DINGUX)
			sprintf(data->system,"dingux");
	#elif defined(GCW)	
		sprintf(data->system,"gcw");
	#elif defined(PANDORA)	
		sprintf(data->system,"pandora");
	#elif defined(WIN32)
		sprintf(data->system,"win32");
	#else
		sprintf(data->system,"linux");
	#endif
	task->dataPointer = data;
	task->timeOut = timeOut;
	task->threadStatus = 1;
	#ifdef _MSC_VER
		task->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
	#else
		task->thread = SDL_CreateThread(spNetC4AUberThread,data);
	#endif
	return task;
}

PREFIX void spNetC4ACopyScoreList(spNetC4AScorePointer* scoreList,spNetC4AScorePointer* newList)
{
	if (scoreList == NULL)
		return;
	if (newList == NULL)
		return;
	spNetC4AScorePointer mom = *scoreList;
	spNetC4AScorePointer last = NULL;
	while (mom)
	{
		spNetC4AScorePointer copy_score = (spNetC4AScorePointer)malloc(sizeof(spNetC4AScore));
		sprintf(copy_score->longname,"%s",mom->longname);
		sprintf(copy_score->shortname,"%s",mom->shortname);
		copy_score->score = mom->score;
		copy_score->commitTime = mom->commitTime;
		copy_score->rank = mom->rank;
		if (last)
			last->next = copy_score;
		else
			*newList = copy_score;
		last = copy_score;
		mom = mom->next;
	}
	if (last)
		last->next = NULL;
	else
		*newList = NULL;
}

typedef struct __ScoreNameStruct *__ScoreNamePointer;
typedef struct __ScoreNameStruct {
	char longname[256];
	char shortname[256];
	__ScoreNamePointer next;
} __ScoreName;

PREFIX void spNetC4AMakeScoresUnique(spNetC4AScorePointer* scoreList)
{
	if (scoreList == NULL)
		return;
	spNetC4AScorePointer mom = *scoreList;
	spNetC4AScorePointer before = NULL;
	__ScoreNamePointer name = NULL;
	__ScoreNamePointer searchStart = NULL;
	while (mom)
	{
		//search mom in name:
		__ScoreNamePointer search = searchStart;
		while (search)
		{
			if (strcmp(mom->shortname,search->shortname) == 0 &&
				strcmp(mom->longname,search->longname) == 0 )
				break; //found
			search = search->next;
		}
		if (search) //found -> remove
		{
			spNetC4AScorePointer next = mom->next;
			before->next = next;
			free(mom);
			mom = next;
		}
		else //add
		{
			__ScoreNamePointer add = (__ScoreNamePointer)malloc(sizeof(__ScoreName));
			sprintf(add->longname,"%s",mom->longname);
			sprintf(add->shortname,"%s",mom->shortname);
			add->next = searchStart;
			searchStart = add;
			before = mom;
			mom = mom->next;
		}
	}
	while (searchStart)
	{
		__ScoreNamePointer next = searchStart->next;
		free(searchStart);
		searchStart = next;
	}
}

PREFIX void spNetC4ADeleteScores(spNetC4AScorePointer* scoreList)
{
	if (scoreList == NULL)
		return;
	while (*scoreList)
	{
		spNetC4AScorePointer next = (*scoreList)->next;
		free(*scoreList);
		(*scoreList) = next;
	}
}

void fill_with_random_hex(char* buffer,int count)
{
	int i;
	for (i = 0; i < count; i++)
	{
		int c = rand()%16;
		char cha;
		if (c < 10)
			cha = c+'0';
		else
			cha = c-10+'a';
		buffer[i] = cha;
	}
}

int c4a_create_thread(void* data)
{
	createPointer createData = (createPointer)data;
	spNetIP ip = spNetResolve("skeezix.wallednetworks.com",13001);
	if (ip.address.ipv4 == SP_INVALID_IP)
	{
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}	
	spNetTCPConnection connection = spNetOpenClientTCP(ip);
	if (connection == NULL)
	{
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	char create_string[2048];
	char buffer[2048];
	char prid[37] = "";
	//generating a new, random prid:
	fill_with_random_hex(prid,8);
	prid[ 8]='-';
	fill_with_random_hex(&(prid[ 9]),4);
	prid[13]='-';
	fill_with_random_hex(&(prid[14]),4);
	prid[18]='-';
	fill_with_random_hex(&(prid[19]),4);
	prid[23]='-';
	fill_with_random_hex(&(prid[24]),12);
	prid[36]=0;
	sprintf(create_string,"{\"email\": \"%s\", \"shortname\": \"%s\", \"password\": \"%s\", \"prid\": \"%s\", \"longname\": \"%s\"}",createData->email,createData->shortname,createData->password,prid,createData->longname);
	sprintf(buffer,"PUT /setprofile_1 HTTP/1.1\r\nUser-Agent: sparrowNet/1.0\r\nHost: %i.%i.%i.%i:13001\r\nAccept: */*\r\nContent-Length: %i\r\nExpect: 100-continue\r\n\r\n",ip.address.ipv4_bytes[0],ip.address.ipv4_bytes[1],ip.address.ipv4_bytes[2],ip.address.ipv4_bytes[3],strlen(create_string));
	if (spNetSendTCP(connection,buffer,strlen(buffer)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	if (spNetSendTCP(connection,create_string,strlen(create_string)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	spNetCloseTCP(connection);
	PROFILE_FILENAME_MAKRO
	SDL_RWops *file=SDL_RWFromFile(filename,"wb");
	SDL_RWwrite(file,prid,36,1);
	char c = '\n';
	SDL_RWwrite(file,&c,1,1);
	SDL_RWwrite(file,create_string,strlen(create_string),1);
	SDL_RWclose(file);
	(*(createData->profile)) = (spNetC4AProfilePointer)malloc(sizeof(spNetC4AProfile));
	sprintf((*(createData->profile))->longname,"%s",createData->longname);
	sprintf((*(createData->profile))->shortname,"%s",createData->shortname);
	sprintf((*(createData->profile))->password,"%s",createData->password);
	sprintf((*(createData->profile))->email,"%s",createData->email);
	sprintf((*(createData->profile))->prid,"%s",prid);
	SDL_mutexP(createData->task->statusMutex);
	createData->task->status = SP_C4A_OK;
	SDL_mutexV(createData->task->statusMutex);
	return 0;	
}

PREFIX int spNetC4ACreateProfile(spNetC4AProfilePointer* profile, char* longname,char* shortname,char* password,char* email,int timeOut)
{
	if (profile == NULL)
		return 1;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		createPointer data = (createPointer)malloc(sizeof(createType));
		data->task = spGlobalC4ATask;
		data->function = c4a_create_thread;
		data->profile = profile;
		sprintf(data->longname,"%s",longname);
		sprintf(data->shortname,"%s",shortname);
		sprintf(data->password,"%s",password);
		sprintf(data->email,"%s",email);
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,data);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;
}

int c4a_delete_thread(void* data)
{
	createPointer createData = (createPointer)data;
	spNetIP ip = spNetResolve("skeezix.wallednetworks.com",13001);
	if (ip.address.ipv4 == SP_INVALID_IP)
	{
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}	
	spNetTCPConnection connection = spNetOpenClientTCP(ip);
	if (connection == NULL)
	{
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	char create_string[2048];
	char buffer[2048];
	sprintf(create_string,"{\"email\": \"%s\", \"shortname\": \"%s\", \"password\": \"%s\", \"prid\": \"%s\", \"longname\": \"%s\"}",(*(createData->profile))->email,(*(createData->profile))->shortname,(*(createData->profile))->password,(*(createData->profile))->prid,(*(createData->profile))->longname);
	sprintf(buffer,"PUT /delprofile_1 HTTP/1.1\r\nUser-Agent: sparrowNet/1.0\r\nHost: %i.%i.%i.%i:13001\r\nAccept: */*\r\nContent-Length: %i\r\nExpect: 100-continue\r\n\r\n",ip.address.ipv4_bytes[0],ip.address.ipv4_bytes[1],ip.address.ipv4_bytes[2],ip.address.ipv4_bytes[3],strlen(create_string));
	if (spNetSendTCP(connection,buffer,strlen(buffer)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	if (spNetSendTCP(connection,create_string,strlen(create_string)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	spNetCloseTCP(connection);
	(*(createData->profile)) = NULL;	
	if (createData->deleteFile)
		spNetC4ADeleteProfileFile();
	SDL_mutexP(createData->task->statusMutex);
	createData->task->status = SP_C4A_OK;
	SDL_mutexV(createData->task->statusMutex);
	return 0;
}

PREFIX int spNetC4ADeleteAccount(spNetC4AProfilePointer* profile,int deleteFile,int timeOut)
{
	if (profile == NULL)
		return 1;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		createPointer data = (createPointer)malloc(sizeof(createType));
		data->task = spGlobalC4ATask;
		data->function = c4a_delete_thread;
		data->profile = profile;
		data->deleteFile = deleteFile;
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,c4a_delete_thread);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,c4a_delete_thread);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;
}

PREFIX void spNetC4ADeleteProfileFile()
{
	PROFILE_FILENAME_MAKRO
//Copied from spRemoveFile to avoid dependencies
#ifdef WIN32
	DeleteFile(filename);
#else
	remove(filename);
#endif	
}

int c4a_edit_thread(void* data)
{
	createPointer createData = (createPointer)data;
	spNetIP ip = spNetResolve("skeezix.wallednetworks.com",13001);
	if (ip.address.ipv4 == SP_INVALID_IP)
	{
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}	
	spNetTCPConnection connection = spNetOpenClientTCP(ip);
	if (connection == NULL)
	{
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	char create_string[2048];
	char buffer[2048];
	sprintf(create_string,"{\"email\": \"%s\", \"shortname\": \"%s\", \"password\": \"%s\", \"prid\": \"%s\", \"longname\": \"%s\"}",createData->email,createData->shortname,createData->password,(*(createData->profile))->prid,createData->longname);
	sprintf(buffer,"PUT /setprofile_1 HTTP/1.1\r\nUser-Agent: sparrowNet/1.0\r\nHost: %i.%i.%i.%i:13001\r\nAccept: */*\r\nContent-Length: %i\r\nExpect: 100-continue\r\n\r\n",ip.address.ipv4_bytes[0],ip.address.ipv4_bytes[1],ip.address.ipv4_bytes[2],ip.address.ipv4_bytes[3],strlen(create_string));
	if (spNetSendTCP(connection,buffer,strlen(buffer)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	if (spNetSendTCP(connection,create_string,strlen(create_string)) == 0)
	{
		spNetCloseTCP(connection);
		SDL_mutexP(createData->task->statusMutex);
		createData->task->status = SP_C4A_ERROR;
		SDL_mutexV(createData->task->statusMutex);
		return 1;
	}
	spNetCloseTCP(connection);
	PROFILE_FILENAME_MAKRO
	SDL_RWops *file=SDL_RWFromFile(filename,"wb");
	SDL_RWwrite(file,(*(createData->profile))->prid,strlen((*(createData->profile))->prid),1);
	char c = '\n';
	SDL_RWwrite(file,&c,1,1);
	SDL_RWwrite(file,create_string,strlen(create_string),1);
	SDL_RWclose(file);
	sprintf((*(createData->profile))->longname,"%s",createData->longname);
	sprintf((*(createData->profile))->shortname,"%s",createData->shortname);
	sprintf((*(createData->profile))->password,"%s",createData->password);
	sprintf((*(createData->profile))->email,"%s",createData->email);
	SDL_mutexP(createData->task->statusMutex);
	createData->task->status = SP_C4A_OK;
	SDL_mutexV(createData->task->statusMutex);
	return 0;	
}

PREFIX int spNetC4AEditProfile(spNetC4AProfilePointer* profile,char* longname,char* shortname,char* password,char* email,int timeOut)
{
	if (profile == NULL)
		return 1;
	SDL_mutexP(spGlobalC4ATask->statusMutex);
	if (spGlobalC4ATask->status != SP_C4A_PROGRESS)
	{
		spGlobalC4ATask->status = SP_C4A_PROGRESS;
		SDL_mutexV(spGlobalC4ATask->statusMutex);
		//Starting a background thread, which does the fancy stuff
		createPointer data = (createPointer)malloc(sizeof(createType));
		data->task = spGlobalC4ATask;
		data->function = c4a_edit_thread;
		data->profile = profile;
		sprintf(data->longname,"%s",longname);
		sprintf(data->shortname,"%s",shortname);
		sprintf(data->password,"%s",password);
		sprintf(data->email,"%s",email);
		spGlobalC4ATask->dataPointer = data;
		spGlobalC4ATask->timeOut = timeOut;
		spGlobalC4ATask->threadStatus = 1;
		#ifdef _MSC_VER
			spGlobalC4ATask->thread = SDL_CreateThread((int (__cdecl *)(void *))spNetC4AUberThread,data);
		#else
			spGlobalC4ATask->thread = SDL_CreateThread(spNetC4AUberThread,data);
		#endif
		return 0;
	}
	SDL_mutexV(spGlobalC4ATask->statusMutex);
	return 1;
}

PREFIX int spNetC4AGetStatusParallel(spNetC4ATaskPointer task)
{
	SDL_mutexP(task->statusMutex);
	if (task->threadStatus)
	{
		SDL_mutexV(task->statusMutex);
		return SP_C4A_PROGRESS;
	}
	int status = task->status;
	SDL_mutexV(task->statusMutex);
	return status;
}

PREFIX int spNetC4AGetStatus()
{
	return spNetC4AGetStatusParallel(spGlobalC4ATask);
}

PREFIX void spNetC4ACancelTaskParallel(spNetC4ATaskPointer task)
{
	SDL_mutexP(task->statusMutex);
	if (task->status > 0)
	{
		task->status = SP_C4A_CANCELED;
		SDL_mutexV(task->statusMutex);
		SDL_WaitThread(task->thread,NULL);
	}
	else
		SDL_mutexV(task->statusMutex);
}

PREFIX void spNetC4ACancelTask()
{
	spNetC4ACancelTaskParallel(spGlobalC4ATask);
}

PREFIX int spNetC4AGetTaskResult()
{
	return spGlobalC4ATask->result;
}

PREFIX int spNetC4AGetTaskResultParallel(spNetC4ATaskPointer task)
{
	return task->result;
}

PREFIX int spNetC4AGetTimeOut()
{
	return spGlobalC4ATask->timeOut;
}

PREFIX int spNetC4AGetTimeOutParallel(spNetC4ATaskPointer task)
{
	return task->timeOut;
}
