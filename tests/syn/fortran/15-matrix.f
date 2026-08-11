C FIXED FORM KITCHEN
#define FIXED 1
12345XCONTINUE
      PROGRAM KITCHEN
      INTEGER I
      REAL X
      LOGICAL OK
      CHARACTER*20 PATH
      CHARACTER*20 DQUOTE
      INTEGER BOZ
      PARAMETER (BOZ=Z'FF')
      PATH = 'C:\path\n'
      PATH = 'it''s'
      DQUOTE = "A ""QUOTE"""
      OK = .TRUE. .AND. .NOT. .FALSE.
      X = 1.0D-3 + ABS(2_INT64)
      X = ATOMIC_ADD + C_LOC + EPSILON + IACHAR + MATMUL
      X = X + NEW_LINE + SELECTED_INT_KIND(4) + SIGN(1.0,X)
      X = X .CUSTOM. 1.0
      X = X + 1 ! INLINE
     1X = X + 2
	INTEGER TABFORM
  100 FORMAT('VALUE')
      IF (OK) CALL WORK(X)
      FINAL,PASS,DEFERRED,EXTENDS,KIND,LEN,CODIMENSION,SYNC
	1X = X + 1
      END
                                                                        CARD73

! Sprint 42 deterministic variant: fortran-fixed-15
