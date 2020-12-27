/**
 * This is an example of a badly formatted file.
 * It is used for testing astyle.
*/

    /* bad spaces, bad indentation */
    int foo(float x,int y) {
     float z=x*y;
         return z;
         }

/* Very long function declaration. */
int big_line( int longVariableName, long* variableName, double twiceAsLong, float whenWillItEnd,long anotherOne) {
	int after_tab =   99;
	anotherOne=(int)twiceAsLong + longVariableName + *variableName + twiceAsLong+whenWillItEnd + anotherOne + 3.14159265;
	if(after_tab > longVariableName && *variableName == anotherOne) {
				return 7; }
				else return 99;
				}

int use_switch(int choice) {
		int result = 2;
	switch(choice) {
		case 88: return 7;
		case 99:
		 return 2;
		case 5: {
			int x = 5;
			result = x * choice;
		    }
		        break;
			}
			return result;
   }
