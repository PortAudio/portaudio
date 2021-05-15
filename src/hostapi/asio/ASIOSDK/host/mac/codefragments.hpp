#ifndef __CodeFragments__
#define __CodeFragments__

class CodeFragment;

class CodeFragments
{
public:
	CodeFragments(char *folderName, long fileType, long resType);
	~CodeFragments();

	long getNumFragments() {return numFragments;}
	bool newInstance(long index, unsigned long *cID);
	void removeInstance(long index, unsigned long cID);
	bool getName(long index, char *name);

protected:
	void loadFragments(short folderRef, long fileType, long resType);
	bool setFolder(char *folderName);
	bool openFragmentFolder(void *specs, char *foldername, short *found_vref);
	bool getFrontProcessDirectory(void *specs);
	short getDefVol();

	CodeFragment *root;
	long numFragments;
	short gARefNum;
	short defVol;
};

#endif
