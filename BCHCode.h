struct BCHCode;

struct BCHCode *   BCHCode_New(int p[], int m, int n, int k, int t);
void               BCHCode_Delete(struct BCHCode * BCHCode_data);
void               BCHCode_Encode(struct BCHCode * BCHCode_data, int data[]);
int                BCHCode_Decode(struct BCHCode * BCHCode_data, int recd[]);
