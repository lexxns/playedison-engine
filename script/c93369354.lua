--フィッシュボーグ－ガンナー
function c93369354.initial_effect(c)
	--spsummon
	local e1=Effect.CreateEffect(c)
	e1:SetDescription(aux.Stringid(93369354,0))
	e1:SetCategory(CATEGORY_SPECIAL_SUMMON)
	e1:SetType(EFFECT_TYPE_IGNITION)
	e1:SetRange(LOCATION_GRAVE)
	e1:SetCondition(c93369354.spcon)
	e1:SetCost(c93369354.spcost)
	e1:SetTarget(c93369354.sptg)
	e1:SetOperation(c93369354.spop)
	c:RegisterEffect(e1)
	--synchro custom
	local e2=Effect.CreateEffect(c)
	e2:SetType(EFFECT_TYPE_SINGLE)
	e2:SetCode(EFFECT_SYNCHRO_MATERIAL_CUSTOM)
	e2:SetProperty(EFFECT_FLAG_CANNOT_DISABLE+EFFECT_FLAG_UNCOPYABLE)
	-- modern form: the synchro procedure calls the operation itself and does the
	-- level math, so there is no target and no value to set (see c14943837)
	e2:SetOperation(c93369354.synop)
	c:RegisterEffect(e2)
end
function c93369354.filter(c)
	return c:IsLevelBelow(3) and c:IsFaceup() and c:IsAttribute(ATTRIBUTE_WATER)
end
function c93369354.spcon(e,tp,eg,ep,ev,re,r,rp)
	return Duel.IsExistingMatchingCard(c93369354.filter,tp,LOCATION_MZONE,0,1,nil)
end
function c93369354.spcost(e,tp,eg,ep,ev,re,r,rp,chk)
	if chk==0 then return Duel.IsExistingMatchingCard(Card.IsDiscardable,tp,LOCATION_HAND,0,1,e:GetHandler()) end
	Duel.DiscardHand(tp,Card.IsDiscardable,1,1,REASON_COST+REASON_DISCARD)
end
function c93369354.sptg(e,tp,eg,ep,ev,re,r,rp,chk)
	local c=e:GetHandler()
	if chk==0 then return Duel.GetLocationCount(tp,LOCATION_MZONE)>0
		and c:IsCanBeSpecialSummoned(e,0,tp,false,false) end
	Duel.SetOperationInfo(0,CATEGORY_SPECIAL_SUMMON,c,1,0,0)
end
function c93369354.spop(e,tp,eg,ep,ev,re,r,rp)
	local c=e:GetHandler()
	if not c:IsRelateToEffect(e) then return end
	Duel.SpecialSummon(c,0,tp,tp,false,false,POS_FACEUP)
end
-- "If this card is used as a Synchro Material Monster, all other Synchro
-- Material Monsters must be WATER."  The procedure hands over the tentative
-- material group; rejecting it here is the whole restriction.
function c93369354.synop(e,tsg,ntsg,sg,lv,sc,tp)
	local c=e:GetHandler()
	if sg:IsExists(function(tc) return not tc:IsAttribute(ATTRIBUTE_WATER) end,1,c) then
		return false
	end
	return true
end
