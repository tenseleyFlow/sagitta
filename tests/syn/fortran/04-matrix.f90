#define ENABLED 1
MoDuLe kitchen
  implicit none
  integer, parameter :: n = 2_int64
  integer :: boz = z'FF'
  real(kind=8) :: x = 1.0d-3
  logical :: ok = .TRUE. .AND. .NOT. .FALSE.
contains
  pure recursive function work(value) result(out)
    real, intent(in) :: value
    real :: out
    character(len=*), parameter :: path = 'C:\path\n'
    character(len=*), parameter :: quote = 'it''s'
    character(len=*), parameter :: dquote = "a ""quote"""
    common /shared/ boz
    data boz /1/
    out = atomic_add + c_loc + epsilon + iachar + matmul + new_line
    out = out + selected_int_kind(4) + sign(1.0, out)
    out = out .custom. 1.0
    out = sqrt(value) + &
        & abs(1.0_dp)
    if (ok) call print_value(out) ! note
100 continue
  end function work
end module kitchen

! Sprint 42 deterministic variant: fortran-free-04
