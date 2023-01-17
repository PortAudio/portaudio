
#ifndef PORTAUDIO_QA_PAQA_TOOLS_H
#define PORTAUDIO_QA_PAQA_TOOLS_H

static int gNumPassed = 0; /* Two globals */
static int gNumFailed = 0;

/*------------------- Macros ------------------------------*/
/* Print ERROR if it fails. Tally success or failure. Odd  */
/* do-while wrapper seems to be needed for some compilers. */

#define EXPECT(_exp) \
    do \
    { \
        if ((_exp)) {\
            gNumPassed++; \
        } \
        else { \
            printf("\nERROR - 0x%x - %s for %s\n", result, Pa_GetErrorText(result), #_exp ); \
            gNumFailed++; \
            goto error; \
        } \
    } while(0)

#define ASSERT_EQ(_a, _b) \
    do \
    { \
        if (_a == _b) {\
            gNumPassed++; \
        } \
        else { \
            printf("\nERROR - %d != %d\n", (int) _a, (int) _b ); \
            gNumFailed++; \
            goto error; \
        } \
    } while(0)

#define HOPEFOR(_exp) \
    do \
    { \
        if ((_exp)) {\
            gNumPassed++; \
        } \
        else { \
            printf("\nERROR - 0x%x - %s for %s\n", result, Pa_GetErrorText(result), #_exp ); \
            gNumFailed++; \
        } \
    } while(0)

#endif /* PORTAUDIO_QA_PAQA_TOOLS_H */

