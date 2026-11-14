---- MODULE G9Together ----
\* G9: a Frametop Together room, who is in it and the links between its people.
\*   together/session/session.cpp (what the links bring), wearer.cpp (what a wearer does),
\*   room/invites.h (a code works once), room/seats.h (FreeId), net/quiclinks.cpp (a link's
\*   control stream keeps its order, and a closed link tells its far end after what was sent).
\* One owner and the guests of Order. A link is a call: its caller's end (c) and its callee's
\* end (e), with what is on its way to each. An end that closes says "fin" after what it sent;
\* a network fault loses what was on its way and brings "fin" to both ends, each in its own
\* time. So one end can hold a link its far end has long forgotten.
\* Not modelled: the tailnet's checks (everyone here is on it and honest), versions, wrong
\* guesses' limit, away, panels, avatars, voice (the owner passes those on like the room), the
\* limit on strangers, and time: a wait that runs out can do so whenever nothing is on its way
\* on that link. A slot is used again once both its ends are gone; a guest's Knocked takes
\* waiting callers in slot order, the code in the order they called.
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Order,      \* the guests: of two, the earlier has the lower address and calls the other
          Slots,      \* the links that can be open at once
          Seats,      \* the places guests can have (1 to kPeople - 1)
          MaxJoins,   \* how often guests call the owner, all told
          MaxFaults,  \* links the network breaks
          MaxOpens,   \* rooms the owner opens
          SameSeat    \* TRUE: a headset let in again takes the place of its earlier self (T1's fix). FALSE: before it

VARIABLES
    om,      \* the owner's mode: "idle", "hosting"
    room,    \* the owner's room: [gen, turn, people], people a sequence of [seat, guest]
    opens,   \* rooms opened so far (a room's gen stands for its id)
    codes,   \* invites nobody used yet (0 or 1)
    gm,      \* [guest -> "idle", "calling", "joined"]
    me,      \* [guest -> its seat in the room it is in, or 0]
    copy,    \* [guest -> the room as its owner last told it]
    link,    \* [slot -> a link, or Free]
    joins, faults,
    yes      \* history: <<guest, gen>> the owner let in

vars == <<om, room, opens, codes, gm, me, copy, link, joins, faults, yes>>

TwoGuests == <<"a", "b">>   \* for a config's Order (a config can't spell a sequence)
ThreeGuests == <<"a", "b", "c">>
Guests == {Order[i] : i \in DOMAIN Order}
Owner == "owner"
Index(g) == CHOOSE i \in DOMAIN Order : Order[i] = g
Lower(g, h) == Index(g) < Index(h)

\* cs: "none", "calledOwner", "calledPeer", "member"; ch: the callee's Hello came; cp: the seat
\* of who was called (0: the owner). es: "none", "stranger", "waiting" (a guest's stranger whose
\* Hello came, not yet someone the room lists), "asking", "member"; ep: the caller's seat.
Free == [from |-> "none", to |-> "none", cs |-> "none", ch |-> FALSE, cp |-> 0, es |-> "none", ep |-> 0, toC |-> <<>>, toE |-> <<>>]
NoRoom == [gen |-> 0, turn |-> 0, people |-> <<>>]

Msg(k) == [k |-> k, gen |-> 0, good |-> FALSE, you |-> 0, room |-> NoRoom]
Hello(gen, good) == [Msg("hello") EXCEPT !.gen = gen, !.good = good]
Welcome(seat, R) == [Msg("welcome") EXCEPT !.you = seat, !.room = R]
RoomMsg(R) == [Msg("room") EXCEPT !.room = R]

Tidy(r) == IF r.cs = "none" /\ r.es = "none" THEN Free ELSE r
SayC(r, m) == IF r.cs = "none" THEN r ELSE [r EXCEPT !.toC = Append(@, m)]   \* to the caller's end
SayE(r, m) == IF r.es = "none" THEN r ELSE [r EXCEPT !.toE = Append(@, m)]   \* to the callee's end
\* links_.Close and the link forgotten: the far end hears of it after what was sent
ShutC(r) == Tidy([SayE(r, Msg("fin")) EXCEPT !.cs = "none", !.ch = FALSE, !.cp = 0, !.toC = <<>>])
ShutE(r) == Tidy([SayC(r, Msg("fin")) EXCEPT !.es = "none", !.ep = 0, !.toE = <<>>])

At(R, s) == {i \in DOMAIN R.people : R.people[i].seat = s}
Listed(R, s, g) == \E i \in DOMAIN R.people : R.people[i] = [seat |-> s, guest |-> g]
Without(R, s) == LET Keep(p) == p.seat # s IN [R EXCEPT !.people = SelectSeq(@, Keep)]
Bump(R) == [R EXCEPT !.turn = @ + 1]
FreeSeats(R) == {s \in Seats : At(R, s) = {}}
Lowest(S) == CHOOSE s \in S : \A t \in S : s <= t

\* LinkOf(person) >= 0, for a guest
Has(L, g, s) == \E l \in Slots :
    \/ L[l].from = g /\ L[l].cs \in {"member", "calledPeer"} /\ L[l].cp = s
    \/ L[l].to = g /\ L[l].es = "member" /\ L[l].ep = s
HasFree(L) == \E l \in Slots : L[l] = Free
FreeSlot(L) == Lowest({l \in Slots : L[l] = Free})

\* Changed(): the room goes to every member's link
Tell(L, R) == [l \in Slots |-> IF L[l].to = Owner /\ L[l].es = "member" THEN SayC(L[l], RoomMsg(R)) ELSE L[l]]
\* The owner's Remove(person) and Changed()
OwnerLoses(L, s) == IF At(room, s) = {} THEN room' = room /\ link' = L
                    ELSE LET R == Bump(Without(room, s)) IN room' = R /\ link' = Tell(L, R)

\* End(): every link of a guest closed, the room forgotten
EndLinks(L, g) == [l \in Slots |-> IF L[l].from = g /\ L[l].cs # "none" THEN ShutC(L[l])
                                   ELSE IF L[l].to = g /\ L[l].es # "none" THEN ShutE(L[l]) ELSE L[l]]
GuestEnds(L, g) == /\ gm' = [gm EXCEPT ![g] = "idle"] /\ me' = [me EXCEPT ![g] = 0]
                   /\ copy' = [copy EXCEPT ![g] = NoRoom] /\ link' = EndLinks(L, g)

Init == /\ om = "idle" /\ room = NoRoom /\ opens = 0 /\ codes = 0
        /\ gm = [g \in Guests |-> "idle"] /\ me = [g \in Guests |-> 0] /\ copy = [g \in Guests |-> NoRoom]
        /\ link = [l \in Slots |-> Free] /\ joins = 0 /\ faults = 0 /\ yes = {}

\* ---- what the owner does (wearer.cpp)
Open == /\ om = "idle" /\ opens < MaxOpens
        /\ om' = "hosting" /\ opens' = opens + 1 /\ room' = [gen |-> opens + 1, turn |-> 0, people |-> <<>>]
        /\ UNCHANGED <<codes, gm, me, copy, link, joins, faults, yes>>

Invite == /\ om = "hosting" /\ codes = 0 /\ codes' = 1
          /\ UNCHANGED <<om, room, opens, gm, me, copy, link, joins, faults, yes>>

\* Answer(ask, accept). With SameSeat, a headset the room lists already goes first: Drop(its
\* link), or Remove(its place) when it has none.
Answer(l, ok) ==
    /\ om = "hosting" /\ link[l].to = Owner /\ link[l].es = "asking"
    /\ LET r == link[l]
           g == r.from
           old == IF SameSeat THEN {room.people[i].seat : i \in {j \in DOMAIN room.people : room.people[j].guest = g}} ELSE {}
           L0 == [k \in Slots |-> IF link[k].to = Owner /\ link[k].es = "member" /\ link[k].ep \in old THEN ShutE(link[k]) ELSE link[k]]
           R0 == IF old = {} THEN room ELSE LET Keep(p) == p.seat \notin old IN Bump([room EXCEPT !.people = SelectSeq(@, Keep)])
           L1 == IF old = {} THEN L0 ELSE Tell(L0, R0)
       IN IF ~ok \/ FreeSeats(R0) = {}
          THEN /\ link' = [link EXCEPT ![l] = ShutE(SayC(r, Msg("refuse")))]
               /\ UNCHANGED <<room, yes>>
          ELSE LET s == Lowest(FreeSeats(R0))
                   R == Bump([R0 EXCEPT !.people = Append(@, [seat |-> s, guest |-> g])])
                   L2 == Tell(L1, R)   \* to those already in: the newcomer's copy is their welcome
               IN /\ room' = R /\ yes' = yes \cup {<<g, R.gen>>}
                  /\ link' = [L2 EXCEPT ![l] = SayC([@ EXCEPT !.es = "member", !.ep = s], Welcome(s, R))]
    /\ UNCHANGED <<om, opens, codes, gm, me, copy, joins, faults>>

\* Leave(): its owner leaving ends the room for everyone
OwnerLeave ==
    /\ om = "hosting"
    /\ link' = [l \in Slots |-> IF link[l].to # Owner \/ link[l].es = "none" THEN link[l]
                                ELSE IF link[l].es = "member" THEN ShutE(SayC(link[l], Msg("leave")))
                                ELSE IF link[l].es = "asking" THEN ShutE(SayC(link[l], Msg("refuse")))
                                ELSE ShutE(link[l])]
    /\ om' = "idle" /\ room' = NoRoom /\ codes' = 0
    /\ UNCHANGED <<opens, gm, me, copy, joins, faults, yes>>

\* ---- what a guest does
\* Join(host, port, code): good says the code is one the owner made that nobody used yet
Join(g, good) ==
    /\ gm[g] = "idle" /\ joins < MaxJoins /\ HasFree(link)
    /\ link' = [link EXCEPT ![FreeSlot(link)] = [Free EXCEPT !.from = g, !.to = Owner, !.cs = "calledOwner", !.es = "stranger",
                                                             !.toE = <<Hello(0, good)>>]]
    /\ gm' = [gm EXCEPT ![g] = "calling"] /\ joins' = joins + 1
    /\ UNCHANGED <<om, room, opens, codes, me, copy, faults, yes>>

GuestLeave(g) ==
    /\ gm[g] # "idle"
    /\ GuestEnds([l \in Slots |-> IF link[l].from = g /\ link[l].cs = "member" THEN SayE(link[l], Msg("leave"))
                                  ELSE IF link[l].to = g /\ link[l].es = "member" THEN SayC(link[l], Msg("leave")) ELSE link[l]], g)
    /\ UNCHANGED <<om, room, opens, codes, joins, faults, yes>>

\* Mesh(): the other guests are called, by the one of each two whose address is lower
MeshWanted(g, i) ==
    /\ gm[g] = "joined" /\ i \in DOMAIN copy[g].people
    /\ LET p == copy[g].people[i] IN p.seat # me[g] /\ Lower(g, p.guest) /\ ~Has(link, g, p.seat)
MeshCall(g, i) ==
    /\ MeshWanted(g, i) /\ HasFree(link)
    /\ LET p == copy[g].people[i] IN
         link' = [link EXCEPT ![FreeSlot(link)] = [Free EXCEPT !.from = g, !.to = p.guest, !.cs = "calledPeer", !.cp = p.seat,
                                                                  !.es = "stranger", !.toE = <<Hello(copy[g].gen, FALSE)>>]]
    /\ UNCHANGED <<om, room, opens, codes, gm, me, copy, joins, faults, yes>>

\* ---- what a link brings the owner (Handle, OnHello, Asked, Down)
DeliverO(l) ==
    /\ link[l].to = Owner /\ link[l].toE # <<>>
    /\ LET r == link[l]
           m == Head(r.toE)
           r1 == [r EXCEPT !.toE = Tail(@)]
           asking == Cardinality({k \in Slots : link[k].to = Owner /\ link[k].es = "asking"})
           \* with SameSeat, a headset the room lists already asks for the place it has
           again == IF SameSeat /\ (\E i \in DOMAIN room.people : room.people[i].guest = r.from) THEN 1 ELSE 0
       IN IF m.k = "hello" /\ r.es = "stranger"
          THEN IF om = "hosting" /\ (Len(room.people) - again) + asking < Cardinality(Seats) /\ m.good /\ codes > 0
               THEN /\ link' = [link EXCEPT ![l] = SayC([r1 EXCEPT !.es = "asking"], Hello(room.gen, FALSE))]
                    /\ codes' = codes - 1 /\ room' = room
               ELSE /\ link' = [link EXCEPT ![l] = ShutE(SayC(r1, Msg("refuse")))]   \* not open, full, or no good code
                    /\ UNCHANGED <<codes, room>>
          ELSE \* a member's Leave, the link down, or something it may not say: Drop, and Down's Remove
               /\ codes' = codes
               /\ IF r.es = "member" /\ om = "hosting" THEN OwnerLoses([link EXCEPT ![l] = ShutE(r1)], r.ep)
                  ELSE link' = [link EXCEPT ![l] = ShutE(r1)] /\ room' = room
    /\ UNCHANGED <<om, opens, gm, me, copy, joins, faults, yes>>

\* ---- what a link brings a guest
\* OnRoom: links to people the room no longer lists go
Gone(L, g, R) == [l \in Slots |->
    IF L[l].from = g /\ L[l].to # Owner /\ L[l].cs \in {"member", "calledPeer"} /\ ~Listed(R, L[l].cp, L[l].to) THEN ShutC(L[l])
    ELSE IF L[l].to = g /\ L[l].es = "member" /\ ~Listed(R, L[l].ep, L[l].from) THEN ShutE(L[l]) ELSE L[l]]

\* Knocked: a waiting caller is the first person the room lists at their address that has no link yet
Knock(L, g, R, l) ==
    LET c == {i \in DOMAIN R.people : R.people[i].seat # me[g] /\ R.people[i].guest = L[l].from /\ ~Has(L, g, R.people[i].seat)}
    IN IF c = {} THEN L
       ELSE [L EXCEPT ![l] = SayC([@ EXCEPT !.es = "member", !.ep = R.people[Lowest(c)].seat], Hello(R.gen, FALSE))]
RECURSIVE KnockAll(_, _, _, _)
KnockAll(L, g, R, todo) ==
    IF todo = {} THEN L
    ELSE LET l == Lowest(todo) IN KnockAll(IF L[l].to = g /\ L[l].es = "waiting" THEN Knock(L, g, R, l) ELSE L, g, R, todo \ {l})

\* at the end that called
DeliverC(l) ==
    /\ link[l].toC # <<>>
    /\ LET r == link[l]
           g == r.from
           m == Head(r.toC)
           r1 == [r EXCEPT !.toC = Tail(@)]
           L1 == [link EXCEPT ![l] = r1]
           same == UNCHANGED <<gm, me, copy>>
       IN IF r.cs = "calledOwner" /\ m.k = "hello" /\ ~r.ch
          THEN link' = [link EXCEPT ![l] = [r1 EXCEPT !.ch = TRUE]] /\ same
          ELSE IF r.cs = "calledOwner" /\ m.k = "welcome" /\ r.ch
          THEN /\ gm' = [gm EXCEPT ![g] = "joined"] /\ me' = [me EXCEPT ![g] = m.you] /\ copy' = [copy EXCEPT ![g] = m.room]
               /\ link' = [link EXCEPT ![l] = [r1 EXCEPT !.cs = "member"]]
          ELSE IF r.cs = "member" /\ r.to = Owner /\ m.k = "room" /\ m.room.gen = copy[g].gen /\ m.room.turn <= copy[g].turn
          THEN link' = L1 /\ same
          ELSE IF r.cs = "member" /\ r.to = Owner /\ m.k = "room" /\ m.room.gen = copy[g].gen /\ At(m.room, me[g]) # {}
          THEN /\ copy' = [copy EXCEPT ![g] = m.room] /\ link' = KnockAll(Gone(L1, g, m.room), g, m.room, Slots)
               /\ UNCHANGED <<gm, me>>
          ELSE IF r.cs = "calledOwner" \/ (r.cs = "member" /\ r.to = Owner)
          THEN GuestEnds(L1, g)   \* refused, the room closed, taken out, or the link to its owner lost
          ELSE IF r.cs = "calledPeer" /\ m.k = "hello" /\ m.gen = copy[g].gen
          THEN link' = [link EXCEPT ![l] = [r1 EXCEPT !.cs = "member", !.ch = TRUE]] /\ same
          ELSE link' = [link EXCEPT ![l] = ShutC(r1)] /\ same
    /\ UNCHANGED <<om, room, opens, codes, joins, faults, yes>>

\* at a guest that was called
DeliverE(l) ==
    /\ link[l].to \in Guests /\ link[l].toE # <<>>
    /\ LET r == link[l]
           g == r.to
           m == Head(r.toE)
           r1 == [r EXCEPT !.toE = Tail(@)]
       IN IF r.es = "stranger" /\ m.k = "hello"
          THEN IF gm[g] = "joined" /\ m.gen = copy[g].gen
               THEN link' = Knock([link EXCEPT ![l] = [r1 EXCEPT !.es = "waiting"]], g, copy[g], l)
               ELSE link' = [link EXCEPT ![l] = ShutE(SayC(r1, Msg("refuse")))]   \* no room here, or another
          ELSE link' = [link EXCEPT ![l] = ShutE(r1)]
    /\ UNCHANGED <<om, room, opens, codes, gm, me, copy, joins, faults, yes>>

\* ---- Tick: what waited too long ends
AskTimeout(l) ==
    /\ link[l].to = Owner /\ link[l].es = "asking" /\ link[l].toE = <<>>
    /\ link' = [link EXCEPT ![l] = ShutE(SayC(link[l], Msg("refuse")))]
    /\ UNCHANGED <<om, room, opens, codes, gm, me, copy, joins, faults, yes>>

HelloTimeout(l) ==
    /\ link[l].toC = <<>> /\ link[l].toE = <<>>
    /\ \/ /\ link[l].es \in {"stranger", "waiting"}
          /\ link' = [link EXCEPT ![l] = ShutE(link[l])] /\ UNCHANGED <<gm, me, copy>>
       \/ /\ link[l].cs = "calledPeer"
          /\ link' = [link EXCEPT ![l] = ShutC(link[l])] /\ UNCHANGED <<gm, me, copy>>
       \/ /\ link[l].cs = "calledOwner"
          /\ GuestEnds(link, link[l].from)
    /\ UNCHANGED <<om, room, opens, codes, joins, faults, yes>>

\* ---- the network: what was on its way is lost, and each end hears of it in its own time
Fault(l) ==
    /\ link[l].cs # "none" /\ link[l].es # "none" /\ faults < MaxFaults /\ faults' = faults + 1
    /\ link' = [link EXCEPT ![l] = [@ EXCEPT !.toC = <<Msg("fin")>>, !.toE = <<Msg("fin")>>]]
    /\ UNCHANGED <<om, room, opens, codes, gm, me, copy, joins, yes>>

You == Open \/ Invite \/ OwnerLeave \/ (\E l \in Slots, ok \in BOOLEAN : Answer(l, ok))
       \/ (\E g \in Guests : GuestLeave(g) \/ \E good \in BOOLEAN : Join(g, good))
Net == \E l \in Slots : Fault(l)
Code == \/ \E l \in Slots : DeliverO(l) \/ DeliverC(l) \/ DeliverE(l) \/ AskTimeout(l) \/ HelloTimeout(l)
        \/ \E g \in Guests : \E i \in 1..Cardinality(Seats) : MeshCall(g, i)
Next == You \/ Net \/ Code

\* Fairness for the code's own steps only: the wearers and the network get none.
Spec == /\ Init /\ [][Next]_vars
        /\ \A l \in Slots : /\ WF_vars(DeliverO(l)) /\ WF_vars(DeliverC(l)) /\ WF_vars(DeliverE(l))
                            /\ WF_vars(AskTimeout(l)) /\ WF_vars(HelloTimeout(l))
        /\ \A g \in Guests : WF_vars(\E i \in 1..Cardinality(Seats) : MeshCall(g, i))

\* ---- rules
TypeOK == /\ om \in {"idle", "hosting"} /\ codes \in 0..1
          /\ \A g \in Guests : gm[g] \in {"idle", "calling", "joined"} /\ (gm[g] = "joined") = (me[g] # 0)
          /\ \A l \in Slots : /\ link[l].cs \in {"none", "calledOwner", "calledPeer", "member"}
                              /\ link[l].es \in {"none", "stranger", "waiting", "asking", "member"}
                              /\ (link[l].toC # <<>> => link[l].cs # "none") /\ (link[l].toE # <<>> => link[l].es # "none")

\* Nobody is in a room, by the owner's count or their own, that the owner didn't let in
InByAcceptOnly == /\ \A i \in DOMAIN room.people : <<room.people[i].guest, room.gen>> \in yes
                  /\ \A g \in Guests : gm[g] = "joined" => <<g, copy[g].gen>> \in yes

\* A headset is in the room once
OnceInRoom == \A i, j \in DOMAIN room.people : room.people[i].guest = room.people[j].guest => i = j

\* A place in the room has one person
OneEachSeat == \A i, j \in DOMAIN room.people : room.people[i].seat = room.people[j].seat => i = j

\* LinkOf is one link: a guest has no two links to one person, the owner no two to one seat
OneLinkEach == /\ \A g \in Guests, s \in Seats \cup {0} :
                     Cardinality({l \in Slots : \/ link[l].from = g /\ link[l].cs \in {"member", "calledPeer"} /\ link[l].cp = s
                                                \/ link[l].to = g /\ link[l].es = "member" /\ link[l].ep = s}) <= 1
               /\ \A s \in Seats : Cardinality({l \in Slots : link[l].to = Owner /\ link[l].es = "member" /\ link[l].ep = s}) <= 1

\* A guest holds someone for one of the room only if the room, as the owner told it, lists them there
PeerIsListed == \A l \in Slots :
    /\ (link[l].to \in Guests /\ link[l].cs \in {"member", "calledPeer"}) => Listed(copy[link[l].from], link[l].cp, link[l].to)
    /\ (link[l].to \in Guests /\ link[l].es = "member") => Listed(copy[link[l].to], link[l].ep, link[l].from)

\* The owner's member links are the room's people
MemberIsListed == \A l \in Slots : (link[l].to = Owner /\ link[l].es = "member") => (om = "hosting" /\ Listed(room, link[l].ep, link[l].from))

\* What a guest has of the room is the owner's, now or earlier
CopyIsOwners == \A g \in Guests : gm[g] = "joined" =>
    /\ copy[g].gen <= opens
    /\ (om = "hosting" /\ copy[g].gen = room.gen) => copy[g].turn <= room.turn

\* Nothing on its way, nobody waiting for an answer, no call owed
Quiet == /\ \A l \in Slots : /\ link[l].toC = <<>> /\ link[l].toE = <<>>
                             /\ link[l].cs \in {"none", "member"} /\ link[l].es \in {"none", "member"}
         /\ \A g \in Guests, i \in 1..Cardinality(Seats) : ~MeshWanted(g, i)

Between(g, h) == {l \in Slots : (link[l].from = g /\ link[l].to = h) \/ (link[l].from = h /\ link[l].to = g)}

\* Everyone agrees who is in the room, and each two of them have one link, whole at both ends
Agreed ==
    /\ \A g \in Guests : gm[g] # "calling"
    /\ \A g \in Guests : gm[g] = "joined" => om = "hosting" /\ copy[g] = room /\ Listed(room, me[g], g)
    /\ \A i \in DOMAIN room.people : gm[room.people[i].guest] = "joined" /\ me[room.people[i].guest] = room.people[i].seat
    /\ \A g \in Guests : LET mine == {l \in Slots : link[l].from = g /\ link[l].to = Owner} IN
          IF gm[g] = "joined" THEN /\ Cardinality(mine) = 1
                                   /\ \A l \in mine : link[l].cs = "member" /\ link[l].es = "member" /\ link[l].ep = me[g]
          ELSE mine = {}
    /\ \A g, h \in Guests : Lower(g, h) =>
          IF gm[g] = "joined" /\ gm[h] = "joined"
          THEN /\ Cardinality(Between(g, h)) = 1
               /\ \A l \in Between(g, h) : /\ link[l].from = g /\ link[l].cs = "member" /\ link[l].es = "member"
                                           /\ link[l].cp = me[h] /\ link[l].ep = me[g]
          ELSE Between(g, h) = {}

Settled == Quiet => Agreed

\* Liveness. Once the wearers and the network leave it alone, it gets there
Settles == []<>Quiet
====
