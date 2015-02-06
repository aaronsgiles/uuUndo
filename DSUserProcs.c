/**********************************************************************************  Project Name:	DropShell**     File Name:	DSUserProcs.c****   Description:	Specific AppleEvent handlers used by the DropBox***********************************************************************************                       A U T H O R   I D E N T I T Y***********************************************************************************	Initials	Name**	--------	-----------------------------------------------**	LDR			Leonard Rosenthol**	MTC			Marshall Clow**	SCS			Stephan Somogyi***********************************************************************************                      R E V I S I O N   H I S T O R Y***********************************************************************************	  Date		Time	Author	Description**	--------	-----	------	---------------------------------------------**	01/25/92			LDR		Removed the use of const on the userDataHandle**	12/09/91			LDR		Added the new SelectFile userProc**								Added the new Install & DisposeUserGlobals procs**								Modified PostFlight to only autoquit on odoc, not pdoc**	11/24/91			LDR		Added the userProcs for pdoc handler**								Cleaned up the placement of braces**								Added the passing of a userDataHandle**	10/29/91			SCS		Changes for THINK C 5**	10/28/91			LDR		Officially renamed DropShell (from QuickShell)**								Added a bunch of comments for clarification**	10/06/91	00:02	MTC		Converted to MPW C**	04/09/91	00:02	LDR		Added to Projector********************************************************************************/#include "DSGlobals.h"#include "DSUserProcs.h"#include "DSDialogs.h"#include "DSUtilsASG.h"#include <Folders.h>#include <GestaltEqu.h>#include <ImageCompression.h>#include <ctype.h>#include <string.h>#include <stdio.h>//==================================================================================================================================//	Globals//==================================================================================================================================typedef struct odocQueue {	struct odocQueue *next;	FSSpec spec;	Handle data;} odocQueue, *odocQueuePtr;enum {	cacheData = 0x10,	dontCache = 0x20};static Boolean gAborted = false;static char *gInputBuffer = nil, *gOutputBuffer = nil;static char *gInputBufferBase = nil, *gOutputBufferBase = nil;static char *gInputBufferPtr = nil, *gOutputBufferPtr = nil;static ParamBlockRec gInputPB1, gInputPB2;static ParamBlockRec gOutputPB1, gOutputPB2;static HParamBlockRec gCloseOutputPB;static HParamBlockRec gOpenOutputPB;static HParamBlockRec gDeleteInputPB;static short gInputRefNum = 0, gOutputRefNum = 0;static Size gInputBufferLeft = 0, gOutputBufferCount = 0;static Size gTotalSize = 0, gBytesRead = 0, gLinesProcessed = 0;static Boolean gLastInputBuffer = false, gFoundEndCode = false;static odocQueuePtr godocQueue = nil;static IOCompletionUPP gGenericCompletion = nil;Boolean gProcessing = false, gHasCQD = false;enum {	kInputBufferSize = 32L * 1024L,	kOutputBufferSize = 32L * 1024L,	kExtensionMapType = 'E2Ty',	kExtensionMapID = 0};static OSErr OpenInputFile(FSSpecPtr theSpec);static OSErr FillInputBuffer(void);static OSErr CloseInputFile(FSSpecPtr theSpec);static char *GetNextInputLine(Size *length, Boolean *badUU);static char *GetNextUUInputLine(void);static OSErr OpenOutputFile(FSSpecPtr inSpec, FSSpecPtr outSpec, char *outName);static OSErr CreateOutputFile(FSSpecPtr outSpec, char *outName, OSType creator, OSType type);static void MapOutputExtension(char *outname, OSType *creator, OSType *type);static OSErr FlushOutputBuffer(void);static OSErr CloseOutputFile(FSSpecPtr theSpec);static OSErr DecodeUULine(char *line);static void AddToQueue(FSSpec *theSpec, Handle userData);static void ProcessQueue(void);static void DisableMenus(void);static void EnableMenus(void);#if defined(powerc) || defined(__powerc)static pascal void GenericCompletion(ParmBlkPtr paramBlock);#elsestatic pascal void GenericCompletion(void);#endif//==================================================================================================================================//	This routine is called during init time.////	It allows you to install more AEVT Handlers beyond the standard four//==================================================================================================================================#pragma segment Mainpascal void InstallOtherEvents(void){}//==================================================================================================================================//	This routine is called when an OAPP event is received.////	Currently, all it does is set the gOApped flag, so you know that//	you were called initally with no docs, and therefore you shouldn't //	quit when done processing any following odocs.//==================================================================================================================================#pragma segment Mainpascal void OpenApp(void){	gOApped = true;}//==================================================================================================================================//	This routine is called when an QUIT event is received.//	We simply set the global done flag so that the main event loop can//	gracefully exit.  We DO NOT call ExitToShell for two reasons://	1) It is a pretty ugly thing to do, but more importantly//	2) The Apple event manager will get REAL upset!//==================================================================================================================================#pragma segment Mainpascal void QuitApp(void){	gDone = true;}//==================================================================================================================================//	This routine is the first one called when an ODOC or PDOC event is received.//	In this routine you would place code used to setup structures, etc. //	which would be used in a 'for all docs' situation (like "Archive all//	dropped files")////	Obviously, the opening boolean tells you whether you should be opening//	or printing these files based on the type of event recieved.////	userDataHandle is a handle that you can create & use to store your own//	data structs.  This dataHandle will be passed around to the other //	odoc/pdoc routines so that you can get at your data without using//	globals - just like the new StandardFile.  ////	We also return a boolean to tell the caller if you support this type//	of event.  By default, our dropboxes don't support the pdoc, so when//	opening is FALSE, we return FALSE to let the caller send back the//	proper error code to the AEManager.//==================================================================================================================================#pragma segment Mainpascal Boolean PreFlightDocs(Boolean opening, Handle *userDataHandle){	if (!opening) return false;	if (gProcessing) return true;	gAborted = false;	ParamText("\p", "\p", "\p", "\p");	if (GenericProgress(codecProgressOpen, 0, 0) == codecAbortErr) return false;	DisableMenus();	return true;		// we support opening, but not printing - see above}//==================================================================================================================================//	This routine is called for each file passed in the ODOC event.//	//	In this routine you would place code for processing each file/folder/disk that//	was dropped on top of you.//==================================================================================================================================#pragma segment Mainpascal void OpenDoc(FSSpecPtr myFSSPtr, Boolean opening, Handle userDataHandle){	FSSpec outFile;	OSErr theErr;	char *line;	if (gAborted) return;	if (gProcessing) {		AddToQueue(myFSSPtr, userDataHandle);		return;	}	ParamText("\p", "\p", "\p", "\p");	UpdateDialogItem(FrontWindow(), progressText2);	GenericProgress(codecProgressForceUpdatePercent, 0, 0);	theErr = OpenInputFile(myFSSPtr);	if (theErr == noErr) {		gLinesProcessed = 0;		do {			Size length;			Boolean bad;			line = GetNextInputLine(&length, &bad);			if (!(++gLinesProcessed & 0x3f))				if (GenericProgress(codecProgressUpdatePercent, FixRatio((gBytesRead - gInputBufferLeft) >> 8, gTotalSize >> 8), 0) == codecAbortErr) {					gAborted = true;					line = nil;					break;				}		} while (line && (line[0] != 'b' || line[1] != 'e' || line[2] != 'g' || line[3] != 'i' ||					line[4] != 'n' || line[5] != ' '));		if (line) {			theErr = OpenOutputFile(myFSSPtr, &outFile, &line[10]);			if (theErr == noErr) {				do {					line = GetNextUUInputLine();					if (line) theErr = DecodeUULine(line);				} while (theErr == noErr && line);				if (theErr != codecAbortErr) FlushOutputBuffer();				else gAborted = true;				CloseOutputFile(&outFile);			}			GenericProgress(codecProgressForceUpdatePercent, 0x10000, 0);		}		CloseInputFile(myFSSPtr);		ForceFolderUpdate(outFile.vRefNum, outFile.parID);	}}//==================================================================================================================================//	This routine is the last routine called as part of an ODOC event.//	//	In this routine you would place code to process any structures, etc. //	that you setup in the PreflightDocs routine.////	If you created a userDataHandle in the PreFlightDocs routines, this is//	the place to dispose of it since the Shell will NOT do it for you!//==================================================================================================================================#pragma segment Mainpascal void PostFlightDocs(Boolean opening, Handle userDataHandle){	if (gProcessing) return;	ProcessQueue();	GenericProgress(codecProgressClose, 0, 0);	EnableMenus();	if (opening && !gOApped) gDone = true;}//==================================================================================================================================//	This routine is called when the user chooses "Select File�" from the//	File Menu.//	//	Currently it simply calls the new StandardGetFile routine to have the//	user select a single file (any type, numTypes = -1) and then calls the//	SendODOCToSelf routine in order to process it.  //			//	The reason we send an odoc to ourselves is two fold: 1) it keeps the code//	cleaner as all file openings go through the same process, and 2) if events//	are ever recordable, the right things happen (this is called Factoring!)////	Modification of this routine to only select certain types of files, selection//	of multiple files, and/or handling of folder & disk selection is left //	as an exercise to the reader.//==================================================================================================================================pascal void SelectFile(void){	StandardFileReply stdReply;	SFTypeList theTypeList;	UserDialogActivate(FrontWindow(), false);	StandardGetFile(NULL, -1, theTypeList, &stdReply);	UserDialogActivate(FrontWindow(), true);	if (stdReply.sfGood) SendODOCToSelf(&stdReply.sfFile);}//==================================================================================================================================//	This routine is called during the program's initialization and gives you//	a chance to allocate or initialize any of your own globals that your//	dropbox needs.//	//	You return a boolean value which determines if you were successful.//	Returning false will cause DropShell to exit immediately.//==================================================================================================================================pascal Boolean InitUserGlobals(void){	long resp;	if (Gestalt(gestaltQuickdrawVersion, &resp) == noErr && resp) gHasCQD = true;	gInputBufferBase = gInputBuffer = (char *)NewPtr(kInputBufferSize * 2);	if (!gInputBuffer) return false;	gOutputBufferBase = gOutputBuffer = (char *)NewPtr(kOutputBufferSize * 2);	if (!gOutputBuffer) return false;		gInputPB1.ioParam.ioBuffer = gInputBufferBase + 256;	gInputPB1.ioParam.ioReqCount = kInputBufferSize - 512;	gInputPB1.ioParam.ioCompletion = gGenericCompletion;	gInputPB1.ioParam.ioPosMode = fsAtMark + dontCache;	gInputPB1.ioParam.ioPosOffset = 0;	gInputPB2.ioParam.ioBuffer = gInputBufferBase + kInputBufferSize + 256;	gInputPB2.ioParam.ioReqCount = kInputBufferSize - 512;	gInputPB2.ioParam.ioCompletion = gGenericCompletion;	gInputPB2.ioParam.ioPosMode = fsAtMark + dontCache;	gInputPB2.ioParam.ioPosOffset = 0;		gOutputPB1.ioParam.ioBuffer = gOutputBufferBase;	gOutputPB1.ioParam.ioCompletion = gGenericCompletion;	gOutputPB1.ioParam.ioPosMode = fsAtMark + dontCache;	gOutputPB1.ioParam.ioPosOffset = 0;	gOutputPB2.ioParam.ioBuffer = gOutputBufferBase + kOutputBufferSize;	gOutputPB2.ioParam.ioCompletion = gGenericCompletion;	gOutputPB2.ioParam.ioPosMode = fsAtMark + dontCache;	gOutputPB2.ioParam.ioPosOffset = 0;	gDeleteInputPB.ioParam.ioResult = 0;	LoadPrefs();		return true;}//==================================================================================================================================//	This routine is called during the program's cleanup and gives you//	a chance to deallocate any of your own globals that you allocated //	in the above routine.//==================================================================================================================================pascal void DisposeUserGlobals(void){	if (gInputBuffer) DisposePtr(gInputBuffer);	if (gOutputBuffer) DisposePtr(gOutputBuffer);	CloseUserDialog(FrontWindow());	SavePrefs();}//==================================================================================================================================//==================================================================================================================================//==================================================================================================================================//==================================================================================================================================//==================================================================================================================================//==================================================================================================================================pascal void	HandleFileMenu(short id){	if (id == 2) CloseUserDialog(FrontWindow());	else if (id == 4) OpenUserDialog(0, id);}OSErr OpenInputFile(FSSpecPtr theSpec){	static HParamBlockRec openPB;	static ParamBlockRec eofPB;	OSErr theErr;		// set up the local parameter block to do an asynchronous open	openPB.fileParam.ioCompletion = gGenericCompletion;	openPB.fileParam.ioNamePtr = theSpec->name;	openPB.fileParam.ioVRefNum = theSpec->vRefNum;	openPB.ioParam.ioPermssn = fsRdPerm;	openPB.fileParam.ioDirID = theSpec->parID;	openPB.fileParam.ioResult = 1;	theErr = PBHOpenDFAsync(&openPB);		// give time to other applications as this is happening	while (openPB.fileParam.ioResult == 1) GiveTime();	theErr = openPB.fileParam.ioResult;	// save the resulting reference number in a global	gInputPB1.ioParam.ioRefNum = gInputPB2.ioParam.ioRefNum = gInputRefNum = openPB.ioParam.ioRefNum;	if (theErr == noErr) {				// set up the other local parameter block to do an asychronous GetEOF		eofPB.ioParam.ioCompletion = gGenericCompletion;		eofPB.ioParam.ioRefNum = gInputRefNum;		eofPB.ioParam.ioResult = 1;		theErr = PBGetEOFAsync(&eofPB);		// again, give up time to other apps while this happens		while (eofPB.ioParam.ioResult == 1) GiveTime();		theErr = eofPB.ioParam.ioResult;		if (theErr == noErr) {					// if everything went ok, store the size in a global, and reset the input reading parameter blocks			gTotalSize = (long)eofPB.ioParam.ioMisc;			gBytesRead = gLinesProcessed = 0;			gLastInputBuffer = gFoundEndCode = false;			gInputBufferLeft = 0;			gInputPB1.ioParam.ioResult = gInputPB2.ioParam.ioResult = 0;			gInputPB1.ioParam.ioActCount = gInputPB2.ioParam.ioActCount = 0;						// fill the two input buffers right away			return FillInputBuffer();		}	}	return theErr;}OSErr FillInputBuffer(void){	Boolean buffer1 = (!gBytesRead || gInputBufferPtr < gInputBufferBase + kInputBufferSize);	Boolean firstTime = !gBytesRead;	static ParmBlkPtr pb, oppPB;	OSErr theErr = noErr;	// get pointers to the parameter block for this buffer and for the opposite buffer	pb = (buffer1) ? &gInputPB1 : &gInputPB2;	oppPB = (buffer1) ? &gInputPB2 : &gInputPB1;	// copy remaining bytes in this buffer to beginning of next buffer	if (gInputBufferLeft) BlockMove(gInputBufferPtr, oppPB->ioParam.ioBuffer - gInputBufferLeft, gInputBufferLeft);	gInputBufferPtr = oppPB->ioParam.ioBuffer - gInputBufferLeft;	// wait for next buffer to finish, if necessary	while (oppPB->ioParam.ioResult == 1) GiveTime();	if (!firstTime && oppPB->ioParam.ioResult == eofErr && !oppPB->ioParam.ioActCount) gLastInputBuffer = true;	// adjust the count of bytes left and bytes read	gInputBufferLeft += oppPB->ioParam.ioActCount;	gBytesRead += oppPB->ioParam.ioActCount;	// now begin loading into the buffer we just skipped out of	pb->ioParam.ioResult = 1;	theErr = PBReadAsync(pb);	if (theErr == eofErr) theErr = noErr;	// if this is the first time reading, then we wait for this buffer to load, and load in a second buffer	if (firstTime) {		gInputBufferPtr = pb->ioParam.ioBuffer;		while (pb->ioParam.ioResult == 1) GiveTime();		gInputBufferLeft = pb->ioParam.ioActCount;		gBytesRead = pb->ioParam.ioActCount;		oppPB->ioParam.ioResult = 1;		theErr = PBReadAsync(oppPB);		if (theErr == eofErr) theErr = noErr;	}	return theErr;}OSErr CloseInputFile(FSSpecPtr theSpec){	static Boolean first = true;	static HParamBlockRec pb;	static CMovePBRec cpb;	OSErr theErr;		if (!first) while (pb.ioParam.ioResult == 1) GiveTime();	else first = false;	pb.ioParam.ioCompletion = gGenericCompletion;	pb.ioParam.ioRefNum = gInputRefNum;	pb.ioParam.ioResult = 1;	theErr = PBCloseAsync((ParmBlkPtr)&pb);	if (gFoundEndCode) {		if ((*gPrefs)->trashSource == kTrashSource) {			while (pb.ioParam.ioResult == 1) GiveTime();			FSSpec destSpec = *theSpec;			theErr = FindFolder(theSpec->vRefNum, kTrashFolderType, 						kCreateFolder, &destSpec.vRefNum, &destSpec.parID);			if (theErr == noErr) {				short count = 1;				Str63 oldName;				BlockMove(theSpec->name, oldName, *theSpec->name + 1);				do {					cpb.ioCompletion = gGenericCompletion;					cpb.ioNamePtr = theSpec->name;					cpb.ioVRefNum = theSpec->vRefNum;					cpb.ioNewName = nil;					cpb.ioNewDirID = destSpec.parID;					cpb.ioDirID = theSpec->parID;					cpb.ioResult = 1;					theErr = PBCatMoveAsync(&cpb);					while (cpb.ioResult == 1) GiveTime();					if (cpb.ioResult == dupFNErr) {						Str63 newName;						if (*theSpec->name < 28) theSpec->name[*theSpec->name + 1] = 0;						else theSpec->name[29] = 0;						sprintf((char *)&newName[1], "%s %d", &oldName[1], ++count);						*newName = strlen((char *)&newName[1]);						pb.fileParam.ioCompletion = gGenericCompletion;						pb.fileParam.ioNamePtr = theSpec->name;						pb.fileParam.ioVRefNum = theSpec->vRefNum;						pb.fileParam.ioDirID = theSpec->parID;						pb.ioParam.ioMisc = (Ptr)newName;						theErr = PBHRenameAsync(&pb);						while (pb.ioParam.ioResult == 1) GiveTime();						BlockMove(newName, theSpec->name, *newName + 1);					}				} while (cpb.ioResult == dupFNErr);			}		} else if ((*gPrefs)->trashSource == kDeleteSource) {			while (pb.ioParam.ioResult == 1) GiveTime();			pb.fileParam.ioCompletion = gGenericCompletion;			pb.fileParam.ioNamePtr = theSpec->name;			pb.fileParam.ioVRefNum = theSpec->vRefNum;			pb.fileParam.ioDirID = theSpec->parID;			theErr = PBHDeleteAsync(&pb);			while (pb.ioParam.ioResult == 1) GiveTime();		}	}	return theErr;}char *GetNextInputLine(Size *length, Boolean *badUU){	OSErr theErr = noErr;	if (gInputBufferLeft <= 0 && gLastInputBuffer) return nil;	else if (gInputBufferLeft < 256) theErr = FillInputBuffer();	if (theErr == noErr) {		register char *start = gInputBufferPtr;		register char *p = gInputBufferPtr;		register char *end = gInputBufferPtr + gInputBufferLeft;		register char c;		do {			c = *p++;			if (c == 0x0d || c == 0x0a) break;			if (c < ' ' || c > '`') *badUU = true;		} while (p < end);		p[-1] = 0;		*length = p - start - 1;		do {			c = *p++;		} while (c == 0x0d && c == 0x0a && p < end);		p--;		gInputBufferPtr = p;		gInputBufferLeft -= (p - start);		return start;	}	return nil;}char *GetNextUUInputLine(void){	register char *line;	Boolean bad = false;	Size length;	do {		register int count, min, max;		line = GetNextInputLine(&length, &bad);		if (!line) return nil;		if (line[0] == 'e' && line[1] == 'n' && line[2] == 'd' && line[3] == 0) {			gFoundEndCode = true;			return nil;		} 		if (bad) {			bad = false;			continue;		}		count = (line[0] - ' ');		if (count < 0 || count > 45) continue;		min = ((count << 2) + 2) / 3;		max = (((min + 3) >> 2) << 2) + 1;		length--;		if (length < min || length > max) continue;		return line;	} while (line);	return nil;}OSErr DecodeUULine(char *line){	register int count = (*line++) - ' ';	register int i;	register char *src, *dst;	register char c1, c2;	OSErr theErr = noErr;	if ((gOutputBufferCount + count + 256 + 3) > kOutputBufferSize) theErr = FlushOutputBuffer();	for (i = 0, src = line, dst = gOutputBufferPtr; i < count; i += 3) {		c1 = (*src++ - ' ') & 0x3f;		c2 = (*src++ - ' ') & 0x3f;		*dst++ = ((c1 << 2) | (c2 >> 4)) & 0xff;		c1 = (*src++ - ' ') & 0x3f;		*dst++ = ((c2 << 4) | (c1 >> 2)) & 0xff;		c2 = (*src++ - ' ') & 0x3f;		*dst++ = ((c1 << 6) | c2) & 0xff;	}	gOutputBufferCount += count;	gOutputBufferPtr = dst;	if (!(++gLinesProcessed & 0x3f))		theErr = GenericProgress(codecProgressUpdatePercent, FixRatio((gBytesRead - gInputBufferLeft) >> 8, gTotalSize >> 8), 0);	return theErr;}OSErr OpenOutputFile(FSSpecPtr inSpec, FSSpecPtr outSpec, char *outName){	register char *p = outName;	register int count = 0;	OSErr theErr = noErr;	OSType creator, type;	register char c;	// first figure out the creator and type based on the output filename (assume the eol is already set up)	MapOutputExtension(outName, &creator, &type);	// now copy the filename into the FSSpec, limiting the length to 31 characters	*outSpec = *inSpec;	outSpec->name[31] = 0;	do {		c = *p++;	} while (c && ++count < 32);	p[-1] = 0;	strncpy((char *)&outSpec->name[1], outName, 30);	*outSpec->name = strlen((char *)&outSpec->name[1]);	ParamText(outSpec->name, "\p", "\p", "\p");	UpdateDialogItem(FrontWindow(), progressText2);	// do the actual creation, avoiding namespace conflicts	theErr = CreateOutputFile(outSpec, outName, creator, type);		// assuming everything else went okay, we finally open the file for writing	if (theErr == noErr) {		gOpenOutputPB.ioParam.ioCompletion = gGenericCompletion;		gOpenOutputPB.fileParam.ioNamePtr = outSpec->name;		gOpenOutputPB.fileParam.ioVRefNum = outSpec->vRefNum;		gOpenOutputPB.ioParam.ioPermssn = fsRdWrPerm;		gOpenOutputPB.fileParam.ioDirID = outSpec->parID;		gOpenOutputPB.ioParam.ioResult = 1;		theErr = PBHOpenDFAsync(&gOpenOutputPB);	}	// reset all the output-related variables, and return	gOutputBufferPtr = gOutputBuffer;	gOutputBufferCount = 0;	gOutputPB1.ioParam.ioResult = gOutputPB2.ioParam.ioResult = 0;	return theErr;}#define ASYNC_CREATE 1OSErr CreateOutputFile(FSSpecPtr outSpec, char *outName, OSType creator, OSType type){	static HParamBlockRec createPB;	static HParamBlockRec fInfoPB;	register int count = 0;	OSErr theErr = noErr;	// here begins our loop to find a valid name for the new output file	do {		#if ASYNC_CREATE			// now set up the local parameter block to do an asynchronous create			createPB.ioParam.ioCompletion = gGenericCompletion;			createPB.fileParam.ioNamePtr = outSpec->name;			createPB.fileParam.ioVRefNum = outSpec->vRefNum;			createPB.fileParam.ioDirID = outSpec->parID;			createPB.ioParam.ioResult = 1;			theErr = PBHCreateAsync(&createPB);						// give time to other processes while this happens			while (createPB.ioParam.ioResult == 1) GiveTime();			if (theErr == noErr) theErr = createPB.ioParam.ioResult;		#else			theErr = FSpCreate(outSpec, creator, type, 0);		#endif				// if a duplicate file name error occurred, bump up the last digit		if (theErr == dupFNErr) {			strncpy((char *)&outSpec->name[1], outName, 28);			outSpec->name[29] = 0;			*outSpec->name = strlen((char *)&outSpec->name[1]);			outSpec->name[++*outSpec->name] = '.';			outSpec->name[++*outSpec->name] = '0' + count++;		}			} while (theErr == dupFNErr);		#if ASYNC_CREATE		// now get the default file information from the Finder		if (theErr == noErr) {			fInfoPB.ioParam.ioCompletion = gGenericCompletion;			fInfoPB.fileParam.ioNamePtr = outSpec->name;			fInfoPB.fileParam.ioVRefNum = outSpec->vRefNum;			fInfoPB.fileParam.ioFDirIndex = 0;			fInfoPB.fileParam.ioDirID = outSpec->parID;			fInfoPB.ioParam.ioResult = 1;			theErr = PBHGetFInfoAsync(&fInfoPB);		}					// give time to other processes while this happens		while (fInfoPB.ioParam.ioResult == 1) GiveTime();		if (theErr == noErr) theErr = fInfoPB.ioParam.ioResult;				// then set the type and creator for this file		if (theErr == noErr) {			fInfoPB.fileParam.ioFlFndrInfo.fdCreator = creator;			fInfoPB.fileParam.ioFlFndrInfo.fdType = type;			fInfoPB.fileParam.ioDirID = outSpec->parID;			fInfoPB.ioParam.ioResult = 1;			theErr = PBHSetFInfoAsync(&fInfoPB);		}				// give time to other processes while this happens		while (fInfoPB.ioParam.ioResult == 1) GiveTime();		if (theErr == noErr) theErr = fInfoPB.ioParam.ioResult;	#endif	return theErr;}void MapOutputExtension(char *outName, OSType *creator, OSType *type){	*creator = '???\?';	*type = '???\?';	if (gMapResource) {		int namelen = strlen(outName);		char *p = *gMapResource;		int count = *(short *)p, i;		p += 2;		while (count--) {			int length = *p++;			for (i = 0; i < length; i++)				if (toupper(outName[namelen - length + i]) != p[i]) break;			if (i == length) {				*type = *(OSType *)&p[5];				*creator = *(OSType *)&p[9];				return;			}			p += 15;		}	}}OSErr FlushOutputBuffer(void){	Boolean buffer1 = (gOutputBufferPtr <= gOutputBufferBase + kOutputBufferSize);	static ParmBlkPtr pb, oppPB;	OSErr theErr = noErr;		// point to the current parameter block and the opposing one	pb = (buffer1) ? &gOutputPB1 : &gOutputPB2;	oppPB = (buffer1) ? &gOutputPB2 : &gOutputPB1;	// make sure that the asynchronous open of the file finished	while (gOpenOutputPB.ioParam.ioResult == 1) GiveTime();	if (gOpenOutputPB.ioParam.ioResult != noErr) return gOpenOutputPB.ioParam.ioResult;	gOutputRefNum = gOpenOutputPB.ioParam.ioRefNum;	// now set up the parameter block for the write and do it	pb->ioParam.ioRefNum = gOutputRefNum;	pb->ioParam.ioReqCount = gOutputBufferCount;	pb->ioParam.ioResult = 1;	theErr = PBWriteAsync(pb);	// reset the counts and the buffer pointers		gOutputBufferCount = 0;	gOutputBufferPtr = oppPB->ioParam.ioBuffer;	// then make sure that the previous write from the next buffer is all done and return	while (oppPB->ioParam.ioResult == 1) GiveTime();	if (oppPB->ioParam.ioResult != noErr) theErr = oppPB->ioParam.ioResult;	return theErr;}OSErr CloseOutputFile(FSSpecPtr theSpec){	OSErr theErr;		gCloseOutputPB.ioParam.ioCompletion = gGenericCompletion;	gCloseOutputPB.ioParam.ioRefNum = gOutputRefNum;	theErr = PBCloseAsync((ParmBlkPtr)&gCloseOutputPB);	if (gAborted) {		static Str255 name;		BlockMove(theSpec->name, name, *theSpec->name + 1);		while (gCloseOutputPB.ioParam.ioResult == 1) GiveTime();		gCloseOutputPB.fileParam.ioCompletion = gGenericCompletion;		gCloseOutputPB.fileParam.ioNamePtr = name;		gCloseOutputPB.fileParam.ioVRefNum = theSpec->vRefNum;		gCloseOutputPB.fileParam.ioDirID = theSpec->parID;		gCloseOutputPB.ioParam.ioResult = 1;		theErr = PBHDeleteAsync(&gCloseOutputPB);	}	return theErr;}void AddToQueue(FSSpec *theSpec, Handle userData){	odocQueuePtr odoc = (odocQueuePtr)NewPtrClear(sizeof(odocQueue)), o;	if (odoc) {		if (godocQueue) {			for (o = godocQueue; o->next; o = o->next);			if (o) o->next = odoc;		} else godocQueue = odoc;		odoc->spec = *theSpec;		odoc->data = userData;	}	}void ProcessQueue(void){	odocQueuePtr o;	while (o = godocQueue) {		OpenDoc(&o->spec, true, o->data);		godocQueue = o->next;		DisposePtr((Ptr)o);	}}void DisableMenus(void){	MenuHandle theMenu = GetMHandle(kAppleNum);	DisableItem(theMenu, 1);	theMenu = GetMHandle(kFileNum);	DisableItem(theMenu, 0);	DrawMenuBar();}void EnableMenus(void){	MenuHandle theMenu = GetMHandle(kAppleNum);	EnableItem(theMenu, 1);	theMenu = GetMHandle(kFileNum);	EnableItem(theMenu, 0);	DrawMenuBar();}#if defined(powerc) || defined(__powerc)pascal void GenericCompletion(ParmBlkPtr paramBlock)#elsepascal void GenericCompletion(void)#endif{}