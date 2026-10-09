--E·HERO プリズマー
-- Elemental HERO Prisma
--
-- Edison ruling (edisonrul.ing / edisonformat.net, and the pre-UTW ruling):
--   * the ① Ignition Effect has **no activation cost** — on resolution Prisma
--     reveals the Fusion Monster *and* sends the named material;
--   * it resolves without effect (nothing is revealed and nothing is sent) if
--     Prisma is not face-up on the field when it resolves;
--   * the send is by effect, so Skill Drain's negation stops it.
-- The reveal/send therefore live in the operation, not in a cost/target check.
function c89312388.initial_effect(c)
	--change name
	local e1=Effect.CreateEffect(c)
	e1:SetDescription(aux.Stringid(89312388,0))
	e1:SetCategory(CATEGORY_TOGRAVE+CATEGORY_DECKDES)
	e1:SetType(EFFECT_TYPE_IGNITION)
	e1:SetCountLimit(1)
	e1:SetRange(LOCATION_MZONE)
	e1:SetTarget(c89312388.target)
	e1:SetOperation(c89312388.operation)
	c:RegisterEffect(e1)
end
function c89312388.filter2(c,fc)
	local fd=c:GetCode()
	if not c:IsAbleToGrave() then return false end
	for i=1,fc.material_count do
		if fd==fc.material[i] then return true end
	end
	return false
end
function c89312388.filter1(c,tp)
	local ct=c.material_count
	return ct~=nil and Duel.IsExistingMatchingCard(c89312388.filter2,tp,LOCATION_DECK,0,1,nil,c)
end
function c89312388.target(e,tp,eg,ep,ev,re,r,rp,chk)
	-- availability only: this must not depend on anything an activation sets
	if chk==0 then return Duel.IsExistingMatchingCard(c89312388.filter1,tp,LOCATION_EXTRA,0,1,nil,tp) end
	Duel.SetOperationInfo(0,CATEGORY_TOGRAVE,nil,1,tp,LOCATION_DECK)
end
function c89312388.operation(e,tp,eg,ep,ev,re,r,rp)
	local c=e:GetHandler()
	-- no cost: if Prisma is gone or face-down on resolution the effect disappears
	if not c:IsRelateToEffect(e) or c:IsFacedown() then return end
	Duel.Hint(HINT_SELECTMSG,tp,HINTMSG_CONFIRM)
	local g=Duel.SelectMatchingCard(tp,c89312388.filter1,tp,LOCATION_EXTRA,0,1,1,nil,tp)
	if g:GetCount()==0 then return end
	Duel.ConfirmCards(1-tp,g)
	Duel.Hint(HINT_SELECTMSG,tp,HINTMSG_TOGRAVE)
	local cg=Duel.SelectMatchingCard(tp,c89312388.filter2,tp,LOCATION_DECK,0,1,1,nil,g:GetFirst())
	if cg:GetCount()==0 or Duel.SendtoGrave(cg,REASON_EFFECT)==0 then return end
	local code=cg:GetFirst():GetCode()
	local e1=Effect.CreateEffect(c)
	e1:SetType(EFFECT_TYPE_SINGLE)
	e1:SetCode(EFFECT_CHANGE_CODE)
	e1:SetProperty(EFFECT_FLAG_CANNOT_DISABLE)
	e1:SetReset(RESET_EVENT+0x1fe0000+RESET_PHASE+PHASE_END)
	e1:SetValue(code)
	c:RegisterEffect(e1)
end
