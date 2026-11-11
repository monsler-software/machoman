extern int local_value(void);
#ifdef INHERITED
extern int inherited_value(void);
#endif
int FUNCTION(void) {
    int value=local_value();
#ifdef INHERITED
    value+=inherited_value();
#endif
    return value;
}
