// very MAC specific; generic code fragment handler
#include "ginclude.h"
#include <macheaders.c>
#include <string.h>
#include <Files.h>
#include <CodeFragments.h>
#include "CodeFragments.hpp"

enum
{
	kNewCFragCopy				= kPrivateCFragCopy
};

class CodeFragmentInstance
{
private:
friend class CodeFragment;
	CodeFragmentInstance() {next = 0; handle = 0; connID = (void*)-1;}
	~CodeFragmentInstance() {CloseConnection((CFragConnectionID*)&connID); DisposeHandle(handle);}

	CodeFragmentInstance *next;
	Handle handle;
	void* connID;
};

class CodeFragment
{
public:
	CodeFragment();
	~CodeFragment();

	bool newInstance(void** cID);
	void removeInstance(void* id);
	bool getName(char *name) {if(!resName[0]) return false; strcpy(name, resName); return true;}

private:
friend class CodeFragments;

	CodeFragment *next;
	CodeFragmentInstance *root;
	Handle handle;
	long numInstances;
	long index;
	char resName[64];
};

//------------------------------------------------------------------------------------------

CodeFragment::CodeFragment()
{
	next = 0;
	root = 0;
	handle = 0;
	numInstances = 0;
	index = 0;
	resName[0] = 0;
}

CodeFragment::~CodeFragment()
{
	// could delete all instances
	// rather omit it...
}

bool CodeFragment::newInstance(void** cID)
{
	Ptr myMainAddr;
	Str255 myErrName;
	char pname[256];

	*cID = (void*)-1;
	if(!handle)
		return false;
	CodeFragmentInstance *c = new CodeFragmentInstance();
	if(!c)
		return false;
	c->handle = this->handle;
	OSErr err = HandToHand(&c->handle);
	if(err == noErr)
	{
		// use fragment loader
		myMainAddr = 0;
		strcpy(pname, resName);
		CtoPstr(pname);
		err = GetMemFragment(*handle, GetHandleSize(handle),
			(unsigned char *)pname, kNewCFragCopy, (CFragConnectionID*)&c->connID,
			&myMainAddr, myErrName);
		if(err == noErr && myMainAddr)
		{
			if(!root)
				root = c;
			else
			{
				CodeFragmentInstance *cc = root;
				while(cc->next)
					cc = cc->next;
				cc->next = c;
			}
			numInstances++;
			*cID = c->connID;
			return true;
		}
	}
	return false;
}

void CodeFragment::removeInstance(void* id)
{
	CodeFragmentInstance *c = root, *last = root;
	while(c)
	{
		if(c->connID == id)
		{
			if(c == root)
				root = root->next;
			else
				last->next = c->next;
			delete c;
			numInstances--;
			return;
		}
		last = c;
		c = c->next;
	}		
}

//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------

CodeFragments::CodeFragments(char *folderName, long fileType, long resType)
{
	root = 0;
	numFragments = 0;
	gARefNum = -1;
	
	if(setFolder(folderName))
	{
		loadFragments(gARefNum, fileType, resType);
		SetVol(0L, defVol);
	}
}

CodeFragments::~CodeFragments()
{
	while(root)
	{
		CodeFragment *c = root->next;
		delete root;
		root = c;
	}
}

bool CodeFragments::newInstance(long index, unsigned long* cID)
{
	*cID = -1;
	CodeFragment *c = root;
	while(c)
	{
		if(c->index == index)
			return c->newInstance((void**)cID);
		c = c->next;
	}
	return false;
}

void CodeFragments::removeInstance(long index, unsigned long cID)
{
	CodeFragment *c = root;
	while(c)
	{
		if(c->index == index)
		{
			c->removeInstance((void*)cID);
			break;
		}
		c = c->next;
	}
}

bool CodeFragments::getName(long index, char *name)
{
	CodeFragment *c = root;
	while(c)
	{
		if(c->index == index)
		{
			strcpy(name, c->resName);
			return true;
		}
		c = c->next;
	}
	return false;
}

//------------------------------------------------------------------------------------
// private

void CodeFragments::loadFragments(short folderRef, long fileType, long resType)
{
	Str255 fileName;
	HFileParam parmblock;
	short resRef,index = 1,lindex;
	short resID;
	ResType resourceType;
	Str255 resName;
	Handle h;
	short curResFile = CurResFile();

	while(true)
	{
		// find any file in the folder
		memset (&parmblock,0,(long)sizeof(HFileParam));
		parmblock.ioNamePtr 	= fileName;
		parmblock.ioVRefNum 	= folderRef;	
		parmblock.ioFDirIndex 	= index;
		if(PBHGetFInfoSync ((HParmBlkPtr)&parmblock) != noErr)
			break;

		// see if the file type matches
		if(parmblock.ioFlFndrInfo.fdType == fileType)
		{
			resRef = OpenResFile(fileName);
			if(ResError()==noErr)
			{
				UseResFile(resRef);
				lindex = 1;
				// search resources in the file found which match
				while(true)
				{
					h = Get1IndResource(resType, lindex);
					if(h)
					{
						CodeFragment *c = new CodeFragment;
						if(!root)
							root = c;
						else
						{
							CodeFragment *cc = root;
							while(cc->next)
								cc = cc->next;
							cc->next = c;
						}
						GetResInfo(h, &resID, &resourceType, resName);
						// PtoCstr(fileName);
						// strcpy(c->fileName, (char *)fileName);
						PtoCstr(resName);
						strcpy(c->resName, (char*)resName);
						DetachResource(h);
						HLockHi(h);
						c->handle = h;
						c->index = numFragments;
						numFragments++;
					}
					else
						break;
					lindex++;
				}
				CloseResFile(resRef);
			}
		}
		index++;
	}
	UseResFile(curResFile);
}

//-----------------------------------------------------------------------------------------

bool CodeFragments::setFolder(char *folderName)
{
	FSSpec fss;

	if(gARefNum == -1)
	{
		if(!getFrontProcessDirectory(&fss))
			return false;
		if(!openFragmentFolder(&fss, folderName, &gARefNum))
			return false;
	}
	defVol = getDefVol();
	SetVol(0L, gARefNum);
	return true;
}

bool CodeFragments::getFrontProcessDirectory(void *specs)
{
	FSSpec *fss = (FSSpec *)specs;
	ProcessInfoRec pif;
	ProcessSerialNumber psn;

	memset(&psn,0,(long)sizeof(ProcessSerialNumber));
	if(GetFrontProcess(&psn) == noErr)
	{
		pif.processName = 0;	//(StringPtr)name;
		pif.processAppSpec = fss;
		pif.processInfoLength = sizeof(ProcessInfoRec);
		if(GetProcessInformation(&psn, &pif) == noErr)
				return true;
	}
	return false;
}

bool CodeFragments::openFragmentFolder(void *specs, char *foldername, short *found_vref)
{
	FSSpec *fss	= (FSSpec *)specs;
	WDPBRec 	funcparms,homefolder;
	HFileInfo 	my_funcfolder;
	char funcFolder_name[32];

	*found_vref = -1;
	strcpy(funcFolder_name,foldername);
	CtoPstr(funcFolder_name);

	memset (&homefolder,0,(long)sizeof(WDPBRec));
	homefolder.ioVRefNum = fss->vRefNum;
	homefolder.ioWDDirID = fss->parID;
	if(PBOpenWDSync(&homefolder) != noErr)
		return false;
 	memset(&my_funcfolder,0,(long)sizeof(CInfoPBRec));
	my_funcfolder.ioNamePtr 	= (StringPtr)funcFolder_name;
	my_funcfolder.ioVRefNum 	= homefolder.ioVRefNum;
	if(PBGetCatInfoSync((CInfoPBPtr)&my_funcfolder) != noErr)
	{
		PBCloseWDSync(&homefolder);
		return false;
	}
	memset (&funcparms,0,(long)sizeof(WDPBRec));
	funcparms.ioWDProcID	= 'stCA';
	funcparms.ioVRefNum = my_funcfolder.ioVRefNum;
	funcparms.ioWDDirID = my_funcfolder.ioDirID;
	if(PBOpenWDSync(&funcparms) != noErr)
	{
		PBCloseWDSync(&homefolder);
		return false;
	}
	PBCloseWDSync(&homefolder);
	*found_vref 	= funcparms.ioVRefNum;
	return true;
}

short CodeFragments::getDefVol()
{
	unsigned char text[32];
	short vol_ref = 0;

	*text=0;
	if(GetVol(text, &vol_ref) == noErr)
		return (vol_ref);
	return 0;
}
