
#ifndef PORTAUDIO_QA_PAQA_TOOLS_H
#define PORTAUDIO_QA_PAQA_TOOLS_H

static int gNumPassed = 0; /* Two globals */
static int gNumFailed = 0;

/*------------------- Macros ------------------------------*/
/* Print ERROR if it fails. Tally success or failure. Odd  */
/* do-while wrapper seems to be needed for some compilers. */

//#define EXPECT(_exp) \
//    do \
//    { \
//        if ((_exp)) {\
//            gNumPassed++; \
//        } \
//        else { \
//            printf("\nERROR - 0x%x - %s for %s\n", result, Pa_GetErrorText(result), #_exp ); \
//            gNumFailed++; \
//            goto error; \
//        } \
//    } while(0)

#define ASSERT_TRUE(_exp) \
    do \
    { \
        if (_exp) {\
            gNumPassed++; \
        } \
        else { \
            printf("ERROR at %s:%d, (%s) not true\n", \
                __FILE__, __LINE__, #_exp ); \
            gNumFailed++; \
            goto error; \
        } \
    } while(0)

#define ASSERT_AB(_a, _b, _op, _opn) \
    do \
    { \
        int mA = (int)(_a); \
        int mB = (int)(_b); \
        if (mA _op mB) {\
            gNumPassed++; \
        } \
        else { \
            printf("ERROR at %s:%d, (%s) %s (%s), %d %s %d\n", \
                __FILE__, __LINE__, #_a, #_opn, #_b, mA, #_opn, mB ); \
            gNumFailed++; \
            goto error; \
        } \
    } while(0)

#define ASSERT_EQ(_a, _b) ASSERT_AB(_a, _b, ==, !=)
#define ASSERT_NE(_a, _b) ASSERT_AB(_a, _b, !=, ==)
#define ASSERT_GT(_a, _b) ASSERT_AB(_a, _b, >, <=)
#define ASSERT_GE(_a, _b) ASSERT_AB(_a, _b, >=, <)
#define ASSERT_LT(_a, _b) ASSERT_AB(_a, _b, <, >=)
#define ASSERT_LE(_a, _b) ASSERT_AB(_a, _b, <=, >)

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

